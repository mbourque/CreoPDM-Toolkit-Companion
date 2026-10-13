/* features.c - /features (non-component) and /relations */
#include "common.h"
#include <ProMenuBar.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProRelSet.h>
#include <ProArray.h>

static ProError nc_filter(ProFeature *feat, ProAppData data)
{
    ProFeattype ft;
    (void)data;
    if (ProFeatureTypeGet(feat, &ft) != PRO_TK_NO_ERROR) return PRO_TK_CONTINUE;
    return ft == PRO_FEAT_COMPONENT ? PRO_TK_CONTINUE : PRO_TK_NO_ERROR;   /* exclude components */
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

static ProError feat_row(ProFeature *feat, ProError status, ProAppData data)
{
    Jw *w = (Jw *)data;
    ProName tn, nm;
    ProLine sub;
    ProFeattype ft = 0;
    char type[96], subtype[96], name[160];
    int supp;
    const char *st;
    (void)status;

    type[0] = subtype[0] = name[0] = 0;
    ProFeatureTypeGet(feat, &ft);
    if (ProFeatureTypenameGet(feat, tn) == PRO_TK_NO_ERROR) w2u(tn, type, sizeof type);
    if (!type[0]) sprintf(type, "FEATTYPE_%d", (int)ft);
    else {                                   /* "Datum Plane" -> "DATUM_PLANE" */
        char *t;
        for (t = type; *t; ++t) *t = (*t == ' ') ? '_' : (char)toupper((unsigned char)*t);
    }
    sub[0] = 0;
    if (ProFeatureSubtypeGet(feat, sub) == PRO_TK_NO_ERROR) w2u(sub, subtype, sizeof subtype);
    if (ProModelitemNameGet((ProModelitem *)feat, nm) == PRO_TK_NO_ERROR) w2u(nm, name, sizeof name);
    if (!name[0]) sprintf(name, "%s (%d)", type, feat->id);   /* same fallback as Creo.JS rows */
    st = status_str(feat, &supp);

    jw_obj(w, NULL);
    jw_int(w, "id", feat->id);
    jw_str(w, "name", name);
    jw_str(w, "type", type);
    jw_str(w, "subtype", subtype);
    jw_null(w, "level");
    jw_str(w, "status", st);
    jw_bool(w, "suppressed", supp);
    jw_end_obj(w);
    return PRO_TK_NO_ERROR;
}

void collect_features(Sb *out, const Ctx *c, const Query *q)
{
    Sb body = { 0 };
    Jw bw, w;
    ProError rc;

    if (c->type == PRO_MDL_DRAWING) { collect_drawing(out, c, q); return; }
    if (c->type != PRO_MDL_PART && c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "features apply to PART/ASSEMBLY/DRAWING; active model is %s%s", c->typeStr, c->type == PRO_MDL_MFG ? " (use /mfg)" : "");
        return;
    }
    jw_init(&bw, &body);
    jw_arr(&bw, NULL);
    rc = ProSolidFeatVisit((ProSolid)c->mdl, feat_row, nc_filter, (ProAppData)&bw);
    jw_end_arr(&bw);
    if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) {
        emit_errorf(out, "ProSolidFeatVisit failed (%d)", (int)rc);
        sb_free(&body);
        return;
    }
    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_raw(&w, "features", body.p);
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
        if (ProModelitemNameGet((ProModelitem *)feat, nm) == PRO_TK_NO_ERROR) jw_wstr(rc->w, "feature_name", nm);
        else jw_null(rc->w, "feature_name");
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
