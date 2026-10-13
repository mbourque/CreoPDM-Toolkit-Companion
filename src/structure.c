/* structure.c - component walker + /structure */
#include "common.h"
#include <ProMenuBar.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProAsmcomp.h>
#include <ProModelitem.h>
#include <ProSkeleton.h>

typedef struct WalkCtx {
    CompCb   cb;
    void    *ud;
    int      recurse;
    int      level;
    int      path[WALK_MAX_DEPTH + 1];
    int      plen;                 /* ids of the owner chain (excl. current comp) */
    ProSolid owner;
    int     *skipped;
} WalkCtx;

static ProError w_filter(ProFeature *feat, ProAppData data)
{
    ProFeattype ft;
    (void)data;
    if (ProFeatureTypeGet(feat, &ft) != PRO_TK_NO_ERROR) return PRO_TK_CONTINUE;
    return ft == PRO_FEAT_COMPONENT ? PRO_TK_NO_ERROR : PRO_TK_CONTINUE;
}

static const char *classify(ProMdl mdl, ProMdlType t)
{
    ProBoolean skel = PRO_B_FALSE;
    if (t == PRO_MDL_ASSEMBLY) return "ASSEMBLY";
    if (mdl && ProMdlIsSkeleton(mdl, &skel) == PRO_TK_NO_ERROR && skel == PRO_B_TRUE)
        return "SKELETON";
    return "PART";
}

static ProError w_visit(ProFeature *feat, ProError status, ProAppData data)
{
    WalkCtx *wc = (WalkCtx *)data;
    CompInfo ci;
    ProMdl mdl = NULL;
    ProMdlType mtype = PRO_MDL_PART;
    ProMdlName tmp;
    ProFamilyMdlName mname;
    ProName featName;
    ProFeatStatus fs;
    char base[256];
    (void)status;

    memset(&ci, 0, sizeof ci);
    mname[0] = 0;

    /* Resolve the referenced model WITHOUT retrieving anything. */
    if (ProAsmcompMdlGet((ProAsmcomp *)feat, &mdl) == PRO_TK_NO_ERROR && mdl) {
        if (ProMdlTypeGet(mdl, &mtype) != PRO_TK_NO_ERROR ||
            ProMdlMdlnameGet(mdl, tmp) != PRO_TK_NO_ERROR) {
            mdl = NULL;
        } else {
            wcsncpy(mname, tmp, PRO_FAMILY_MDLNAME_SIZE - 1);
            mname[PRO_FAMILY_MDLNAME_SIZE - 1] = 0;
        }
    }
    if (!mdl) {
        ProMdlfileType ft;
        if (ProAsmcompMdlMdlnameGet((ProAsmcomp *)feat, &ft, mname) != PRO_TK_NO_ERROR ||
            mname[0] == 0) {
            if (wc->skipped) (*wc->skipped)++;
            return PRO_TK_NO_ERROR;           /* no referenced model: not real */
        }
        mtype = (ft == PRO_MDLFILE_ASSEMBLY) ? PRO_MDL_ASSEMBLY : PRO_MDL_PART;
    }

    w2u(mname, base, sizeof base);
    _snprintf(ci.file, sizeof ci.file, "%s%s", base, mtype == PRO_MDL_ASSEMBLY ? ".asm" : ".prt");
    ci.file[sizeof ci.file - 1] = 0;
    lower_ascii(ci.file);
    ci.kind = classify(mdl, mtype);

    if (ProModelitemNameGet((ProModelitem *)feat, featName) == PRO_TK_NO_ERROR)
        w2u(featName, ci.featName, sizeof ci.featName);

    ci.status = "ACTIVE";
    if (ProFeatureStatusGet(feat, &fs) == PRO_TK_NO_ERROR) {
        switch (fs) {
        case PRO_FEAT_ACTIVE:             ci.status = "ACTIVE"; break;
        case PRO_FEAT_INACTIVE:           ci.status = "INACTIVE"; break;
        case PRO_FEAT_UNREGENERATED:      ci.status = "UNREGENERATED"; break;
        case PRO_FEAT_SUPPRESSED:         ci.status = "SUPPRESSED"; ci.suppressed = 1; break;
        case PRO_FEAT_FAMTAB_SUPPRESSED:  ci.status = "SUPPRESSED_FAMILY_TABLE"; ci.suppressed = 1; break;
        case PRO_FEAT_SIMP_REP_SUPPRESSED:ci.status = "SUPPRESSED_SIMPREP"; ci.suppressed = 1; break;
        case PRO_FEAT_PROG_SUPPRESSED:    ci.status = "SUPPRESSED_PROGRAM"; ci.suppressed = 1; break;
        default:                          ci.status = "UNKNOWN"; break;
        }
    }

    ci.id = feat->id;
    ci.level = wc->level;
    ci.feat = feat;
    ci.owner = wc->owner;
    ci.mdl = mdl;
    memcpy(ci.path, wc->path, sizeof(int) * wc->plen);
    ci.path[wc->plen] = feat->id;
    ci.pathLen = wc->plen + 1;

    wc->cb(&ci, wc->ud);

    /* Recurse only into sub-assemblies already in session. */
    if (wc->recurse && mtype == PRO_MDL_ASSEMBLY && mdl && !ci.suppressed &&
        wc->plen < WALK_MAX_DEPTH) {
        WalkCtx child = *wc;
        child.level = wc->level + 1;
        child.plen = ci.pathLen;
        memcpy(child.path, ci.path, sizeof(int) * ci.pathLen);
        child.owner = (ProSolid)mdl;
        ProSolidFeatVisit((ProSolid)mdl, w_visit, w_filter, (ProAppData)&child);
    }
    return PRO_TK_NO_ERROR;
}

int walk_components(ProSolid root, int recurse, CompCb cb, void *ud, int *skipped)
{
    WalkCtx wc;
    ProError err;
    memset(&wc, 0, sizeof wc);
    wc.cb = cb; wc.ud = ud; wc.recurse = recurse; wc.level = 1;
    wc.owner = root; wc.skipped = skipped;
    err = ProSolidFeatVisit(root, w_visit, w_filter, (ProAppData)&wc);
    if (err == PRO_TK_E_NOT_FOUND) err = PRO_TK_NO_ERROR;   /* no components */
    return (int)err;
}

/* ---------- /structure ---------- */
typedef struct { Jw *w; int count; int recurse; } StructOut;

static void structure_cb(const CompInfo *ci, void *ud)
{
    StructOut *so = (StructOut *)ud;
    Jw *w = so->w;
    char p[256];
    int i, n = 0;

    /* Row shape matches Creo.JS Structure rows: id, name (model tip WITH extension), type (PART |
       ASSEMBLY | SKELETON, never COMPONENT), subtype, level, status, suppressed, path. Only real
       components are walked, so datums / CSYS / cross-sections cannot appear here. */
    p[0] = 0;
    for (i = 0; i < ci->pathLen && n < 230; ++i)
        n += sprintf(p + n, i ? "/%d" : "%d", ci->path[i]);

    jw_obj(w, NULL);
    jw_int(w, "id", ci->id);
    jw_str(w, "name", ci->file);
    jw_str(w, "featureName", ci->featName[0] ? ci->featName : NULL);
    jw_str(w, "type", ci->kind);
    jw_str(w, "subtype", "");
    jw_int(w, "level", ci->level);
    jw_str(w, "status", ci->status);
    jw_bool(w, "suppressed", ci->suppressed);
    jw_str(w, "path", p);
    if (so->recurse)
        jw_bool(w, "expanded", ci->mdl != NULL && strcmp(ci->kind, "ASSEMBLY") == 0 && !ci->suppressed);
    jw_end_obj(w);
    so->count++;
}

void collect_structure(Sb *out, const Ctx *c, const Query *q)
{
    Sb comp = { 0 };
    Jw cw, w;
    StructOut so;
    char v[8];
    int skipped = 0, rc;

    if (c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "structure applies to ASSEMBLY; active model is %s%s", c->typeStr, c->type == PRO_MDL_MFG ? " (use /mfg)" : "");
        return;
    }
    memset(&so, 0, sizeof so);
    so.recurse = (q_get(q, "recurse", v, sizeof v) && v[0] == '1');

    jw_init(&cw, &comp);
    jw_arr(&cw, NULL);
    so.w = &cw;
    rc = walk_components((ProSolid)c->mdl, so.recurse, structure_cb, &so, &skipped);
    jw_end_arr(&cw);
    if (rc != PRO_TK_NO_ERROR) {
        emit_errorf(out, "ProSolidFeatVisit failed (%d)", rc);
        sb_free(&comp);
        return;
    }

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_int(&w, "componentCount", so.count);
    jw_int(&w, "skippedNoModel", skipped);
    jw_bool(&w, "recurse", so.recurse);
    jw_raw(&w, "components", comp.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&comp);
}
