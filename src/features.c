/* features.c - /features (non-component) and /relations */
#include "common.h"
#include <ProMenuBar.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProRelSet.h>
#include <ProUdf.h>
#include <ProGroup.h>
#include <ProArray.h>

static ProError nc_filter(ProFeature *feat, ProAppData data)
{
    ProFeattype ft;
    (void)data;
    if (ProFeatureTypeGet(feat, &ft) != PRO_TK_NO_ERROR) return PRO_TK_CONTINUE;
    if (ft == PRO_FEAT_COMPONENT) return PRO_TK_CONTINUE;           /* components belong to /structure */
    {
        ProBoolean vis = PRO_B_TRUE;                                  /* internal (invisible) features are skipped */
        if (ProFeatureVisibilityGet(feat, &vis) == PRO_TK_NO_ERROR && vis == PRO_B_FALSE) return PRO_TK_CONTINUE;
    }
    return PRO_TK_NO_ERROR;
}

static const char *status_str(ProFeature *feat, int *suppressed)
{
    ProFeatStatus fs;
    *suppressed = 0;
    if (ProFeatureStatusGet(feat, &fs) != PRO_TK_NO_ERROR) return "ACTIVE";
    switch (fs) {
    case PRO_FEAT_ACTIVE:              return "ACTIVE";
    case PRO_FEAT_INACTIVE:            return "INACTIVE";
    case PRO_FEAT_UNREGENERATED:       return "UNREGENERATED";
    case PRO_FEAT_SUPPRESSED:          *suppressed = 1; return "SUPPRESSED";
    case PRO_FEAT_FAMTAB_SUPPRESSED:   *suppressed = 1; return "SUPPRESSED_FAMILY_TABLE";
    case PRO_FEAT_SIMP_REP_SUPPRESSED: *suppressed = 1; return "SUPPRESSED_SIMPREP";
    case PRO_FEAT_PROG_SUPPRESSED:     *suppressed = 1; return "SUPPRESSED_PROGRAM";
    default:                           return "UNKNOWN";
    }
}

/* ---- feature rows, collected first so group members can be nested under their group head ---- */
typedef struct {
    int         id;
    int         parent;          /* id of the containing group head, 0 = none */
    int         is_head;
    char        name[160], type[96], subtype[96];
    const char *status;
    int         suppressed;
    int         emitted;
    int         after_insert;    /* insert mode active and this feature is after the insertion point */
} FRow;

typedef struct { FRow *rows; int n, cap; ProSolid owner; } FCtx;

static ProError feat_row(ProFeature *feat, ProError status, ProAppData data)
{
    FCtx *fc = (FCtx *)data;
    FRow *r;
    ProName tn, nm;
    ProLine sub;
    ProFeattype ft = 0;
    ProGroup grp;
    (void)status;

    if (fc->n >= fc->cap) {
        int ncap = fc->cap ? fc->cap * 2 : 256;
        FRow *nr = (FRow *)realloc(fc->rows, ncap * sizeof(FRow));
        if (!nr) return PRO_TK_NO_ERROR;
        fc->rows = nr;
        fc->cap = ncap;
    }
    r = &fc->rows[fc->n];
    memset(r, 0, sizeof *r);

    ProFeatureTypeGet(feat, &ft);
    if (ProFeatureTypenameGet(feat, tn) == PRO_TK_NO_ERROR) w2u(tn, r->type, sizeof r->type);
    if (!r->type[0]) sprintf(r->type, "FEATTYPE_%d", (int)ft);
    else {                                   /* "Datum Plane" -> "DATUM_PLANE" */
        char *t;
        for (t = r->type; *t; ++t) *t = (*t == ' ') ? '_' : (*t >= 'a' && *t <= 'z') ? (char)(*t - 32) : *t;   /* ASCII-only upper-casing */
    }
    sub[0] = 0;
    if (ProFeatureSubtypeGet(feat, sub) == PRO_TK_NO_ERROR) w2u(sub, r->subtype, sizeof r->subtype);
    if (ProModelitemNameGet((ProModelitem *)feat, nm) == PRO_TK_NO_ERROR) w2u(nm, r->name, sizeof r->name);
    if (is_noname_u(r->name)) return PRO_TK_NO_ERROR;      /* Creo placeholder for hidden unnamed features: never emitted */
    if (!r->name[0]) sprintf(r->name, "%s (%d)", r->type, feat->id);   /* same fallback as Creo.JS rows */
    r->status = status_str(feat, &r->suppressed);
    r->id = feat->id;
    r->is_head = (ft == PRO_FEAT_GROUP_HEAD);

    /* containing group (a group head id), if any; a head can itself sit inside a parent group */
    if (ProFeatureGroupGet(feat, &grp) == PRO_TK_NO_ERROR && grp.id != feat->id) r->parent = grp.id;
    fc->n++;
    return PRO_TK_NO_ERROR;
}

static void emit_frow(Jw *w, FRow *r, int level)
{
    jw_obj(w, NULL);
    jw_int(w, "id", r->id);
    jw_str(w, "name", r->name);
    jw_str(w, "type", r->type);
    jw_str(w, "subtype", r->subtype);
    jw_int(w, "level", level);
    jw_str(w, "status", r->status);
    jw_bool(w, "suppressed", r->suppressed);
    if (r->after_insert) jw_bool(w, "after_insert", 1);     /* only on affected rows (insert mode) */
    jw_end_obj(w);
    r->emitted = 1;
}

/* emit the members of group head `head_id` (in feature order), recursing into nested groups */
static void emit_members(Jw *w, FRow *rows, int n, int head_id, int level, int depth)
{
    int i;
    if (depth > 16) return;
    for (i = 0; i < n; i++) {
        if (rows[i].emitted || rows[i].parent != head_id) continue;
        emit_frow(w, &rows[i], level);
        if (rows[i].is_head) emit_members(w, rows, n, rows[i].id, level + 1, depth + 1);
    }
}

static int is_listed_head(const FRow *rows, int n, int id)
{
    int i;
    for (i = 0; i < n; i++) if (rows[i].id == id && rows[i].is_head) return 1;
    return 0;
}

void collect_features(Sb *out, const Ctx *c, const Query *q)
{
    Sb body = { 0 };
    Jw bw, w;
    FCtx fc;
    ProError rc;
    int i, insert_active = 0, insert_rc = 0, after_id = 0, n_after = 0;
    char after_name[160] = "";

    if (c->type == PRO_MDL_DRAWING) { collect_drawing(out, c, q); return; }
    if (c->type != PRO_MDL_PART && c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "features apply to PART/ASSEMBLY/DRAWING; active model is %s%s", c->typeStr, c->type == PRO_MDL_MFG ? " (use /mfg)" : "");
        return;
    }
    memset(&fc, 0, sizeof fc);
    fc.owner = (ProSolid)c->mdl;
    rc = ProSolidFeatVisit((ProSolid)c->mdl, feat_row, nc_filter, (ProAppData)&fc);
    if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) {
        emit_errorf(out, "ProSolidFeatVisit failed (%d)", (int)rc);
        free(fc.rows);
        return;
    }

    /* Insert mode ("Insert Here"): the features after the insertion point are not part of the model until insert mode
       is cancelled. Toolkit reports them as SUPPRESSED (seen on Creo 13; INACTIVE is handled too) and has no call for the
       insertion point itself, so it is inferred as the last feature that is neither suppressed nor inactive; every row
       after it is marked after_insert. A feature the user suppressed on purpose right at the end of the tree cannot be
       told apart from these (insert_mode.inferred says so). */
    {
        ProBoolean ins = PRO_B_FALSE;
        ProError irc = ProInsertModeIsActive((ProSolid)c->mdl, &ins);
        int last_ok = -1;
        insert_rc = (int)irc;
        insert_active = (irc == PRO_TK_NO_ERROR && ins == PRO_B_TRUE);
        if (insert_active) {
            for (i = 0; i < fc.n; i++)
                if (strncmp(fc.rows[i].status, "SUPPRESSED", 10) != 0 && strcmp(fc.rows[i].status, "INACTIVE") != 0) last_ok = i;
            for (i = 0; i < fc.n; i++) {
                FRow *r = &fc.rows[i];
                if (i > last_ok || strcmp(r->status, "INACTIVE") == 0) { r->after_insert = 1; n_after++; }
            }
            if (last_ok >= 0) { after_id = fc.rows[last_ok].id; strncpy(after_name, fc.rows[last_ok].name, sizeof after_name - 1); }
        }
    }

    /* Tree order: each top-level feature (not inside a listed group) in feature order; a GROUP_HEAD is
       followed by its members at level+1 (and nested groups deeper). A member whose group head is not in
       the list (e.g. hidden) is treated as top level. */
    jw_init(&bw, &body);
    jw_arr(&bw, NULL);
    for (i = 0; i < fc.n; i++) {
        FRow *r = &fc.rows[i];
        if (r->emitted) continue;
        if (r->parent && is_listed_head(fc.rows, fc.n, r->parent)) continue;   /* emitted under its head */
        emit_frow(&bw, r, 1);
        if (r->is_head) emit_members(&bw, fc.rows, fc.n, r->id, 2, 0);
    }
    for (i = 0; i < fc.n; i++)                                                 /* safety: nothing may be lost */
        if (!fc.rows[i].emitted) emit_frow(&bw, &fc.rows[i], 1);
    jw_end_arr(&bw);
    free(fc.rows);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_raw(&w, "features", body.p);
    jw_obj(&w, "insert_mode");                          /* additive: the model is saved with Insert Here active */
    if (insert_rc == (int)PRO_TK_NO_ERROR) {
        jw_bool(&w, "active", insert_active);
        if (insert_active) {
            if (after_id) jw_int(&w, "after_feature_id", after_id); else jw_null(&w, "after_feature_id");
            jw_str(&w, "after_feature_name", after_name[0] ? after_name : NULL);
            jw_int(&w, "inactive_count", n_after);
            jw_bool(&w, "inferred", 1);                 /* insertion point inferred from the feature statuses */
        }
    } else {
        jw_null(&w, "active");                          /* could not read */
    }
    jw_end_obj(&w);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&body);
}

/* ---------- relations ---------- */
/* Writes lines of a relset as a JSON array into *dst. Returns line count (0 = none/failed). */
static int relset_lines(ProRelset *rs, Sb *dst)
{
    ProWstring *lines = NULL;
    Jw w;
    int n = 0, i;
    if (ProArrayAlloc(0, sizeof(ProWstring), 1, (ProArray *)&lines) != PRO_TK_NO_ERROR) return 0;
    if (ProRelsetRelationsGet(rs, &lines) != PRO_TK_NO_ERROR) {
        ProArrayFree((ProArray *)&lines);
        return 0;
    }
    ProArraySizeGet((ProArray)lines, &n);
    jw_init(&w, dst);
    jw_arr(&w, NULL);
    for (i = 0; i < n; i++) jw_wstr(&w, NULL, lines[i]);
    jw_end_arr(&w);
    ProWstringproarrayFree(lines);
    return n;
}

typedef struct { Jw *w; } RelFeatCtx;

static ProError rel_feat_row(ProFeature *feat, ProError status, ProAppData data)
{
    RelFeatCtx *rc = (RelFeatCtx *)data;
    ProRelset rs;
    Sb lines = { 0 };
    ProName nm;
    (void)status;
    if (ProModelitemToRelset((ProModelitem *)feat, &rs) != PRO_TK_NO_ERROR) return PRO_TK_NO_ERROR;
    if (relset_lines(&rs, &lines) > 0) {
        jw_obj(rc->w, NULL);
        jw_int(rc->w, "feature_id", feat->id);
        {
            char fnU[160];
            fnU[0] = 0;
            if (ProModelitemNameGet((ProModelitem *)feat, nm) == PRO_TK_NO_ERROR) w2u(nm, fnU, sizeof fnU);
            if (fnU[0] && !is_noname_u(fnU)) jw_str(rc->w, "feature_name", fnU); else jw_null(rc->w, "feature_name");
        }
        jw_raw(rc->w, "relations", lines.p);
        jw_end_obj(rc->w);
    }
    sb_free(&lines);
    return PRO_TK_NO_ERROR;
}

void collect_relations(Sb *out, const Ctx *c, const Query *q)
{
    Jw w, fw, ew;
    Sb normal = { 0 }, post = { 0 }, feats = { 0 }, errs = { 0 };
    ProModelitem mi;
    ProRelset rs;
    RelFeatCtx rc;
    int nerr = 0;
    (void)q;

    if (c->type != PRO_MDL_PART && c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "relations apply to PART/ASSEMBLY; active model is %s", c->typeStr);
        return;
    }
    jw_init(&ew, &errs);
    jw_arr(&ew, NULL);

    if (ProMdlToModelitem(c->mdl, &mi) == PRO_TK_NO_ERROR &&
        ProModelitemToRelset(&mi, &rs) == PRO_TK_NO_ERROR) {
        relset_lines(&rs, &normal);
    } else {
        jw_str(&ew, NULL, "Relations: relation set not available"); nerr++;
    }
    if (ProRelsetPostregenerationInit(c->mdl, &rs) == PRO_TK_NO_ERROR) {
        relset_lines(&rs, &post);
    } else {
        jw_str(&ew, NULL, "PostRegenerationRelations: not available"); nerr++;
    }

    jw_init(&fw, &feats);
    jw_arr(&fw, NULL);
    rc.w = &fw;
    ProSolidFeatVisit((ProSolid)c->mdl, rel_feat_row, nc_filter, (ProAppData)&rc);
    jw_end_arr(&fw);
    jw_end_arr(&ew);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_raw(&w, "normal", normal.p ? normal.p : "[]");
    jw_raw(&w, "post_regeneration", post.p ? post.p : "[]");
    jw_raw(&w, "features", feats.p);
    jw_raw(&w, "errors", errs.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    (void)nerr;
    sb_free(&normal); sb_free(&post); sb_free(&feats); sb_free(&errs);
}
