/* structure.c - component walker + /structure */
#include "common.h"
#include <ProMenuBar.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProAsmcomp.h>
#include <ProModelitem.h>
#include <ProSkeleton.h>
#include <ProCabling.h>
#include <ProArray.h>

#define MAX_HARN 64
#define MAX_EMIT 512

typedef struct { ProMdl h[MAX_EMIT]; int n; } Seen;   /* harnesses already reported anywhere in this walk */

typedef struct WalkCtx {
    CompCb   cb;
    void    *ud;
    int      recurse;
    int      level;
    int      path[WALK_MAX_DEPTH + 1];
    int      plen;                 /* ids of the owner chain (excl. current comp) */
    ProSolid owner;
    int     *skipped;
    ProMdl   hlist[MAX_HARN];      /* harnesses of the owner assembly (Toolkit cabling list) */
    int      nh;
    ProMdl   emitted[MAX_EMIT];    /* component models already reported for this owner */
    int      ne;
    Seen    *seen;
} WalkCtx;

static ProError w_visit(ProFeature *feat, ProError status, ProAppData data);

static ProError w_filter(ProFeature *feat, ProAppData data)
{
    ProFeattype ft;
    (void)data;
    if (ProFeatureTypeGet(feat, &ft) != PRO_TK_NO_ERROR) return PRO_TK_CONTINUE;
    return ft == PRO_FEAT_COMPONENT ? PRO_TK_NO_ERROR : PRO_TK_CONTINUE;
}

static void fill_status(ProFeature *feat, CompInfo *ci)
{
    ProFeatStatus fs;
    ci->status = "ACTIVE";
    if (ProFeatureStatusGet(feat, &fs) == PRO_TK_NO_ERROR) {
        switch (fs) {
        case PRO_FEAT_ACTIVE:             ci->status = "ACTIVE"; break;
        case PRO_FEAT_INACTIVE:           ci->status = "INACTIVE"; break;
        case PRO_FEAT_UNREGENERATED:      ci->status = "UNREGENERATED"; break;
        case PRO_FEAT_SUPPRESSED:         ci->status = "SUPPRESSED"; ci->suppressed = 1; break;
        case PRO_FEAT_FAMTAB_SUPPRESSED:  ci->status = "SUPPRESSED_FAMILY_TABLE"; ci->suppressed = 1; break;
        case PRO_FEAT_SIMP_REP_SUPPRESSED:ci->status = "SUPPRESSED_SIMPREP"; ci->suppressed = 1; break;
        case PRO_FEAT_PROG_SUPPRESSED:    ci->status = "SUPPRESSED_PROGRAM"; ci->suppressed = 1; break;
        default:                          ci->status = "UNKNOWN"; break;
        }
    }
}

/* Cabling harnesses of one assembly (Toolkit's own list). Failure just leaves the list empty. */
static void load_harnesses(WalkCtx *wc)
{
    ProHarness *arr = NULL;
    int n = 0, i;
    wc->nh = 0;
    wc->ne = 0;
    if (ProAssemblyHarnessesTopCollect((ProAssembly)wc->owner, &arr) == PRO_TK_NO_ERROR && arr) {
        ProArraySizeGet((ProArray)arr, &n);
        for (i = 0; i < n && wc->nh < MAX_HARN; i++) wc->hlist[wc->nh++] = (ProMdl)arr[i];
        ProArrayFree((ProArray *)&arr);
    }
}

/* Is this component a harness? In-memory model: member of the owner's harness list, or its on-disk
   header says HARNESS. Name-only component: its name matches a harness in the list. Never loads. */
static int is_harness_component(WalkCtx *wc, ProMdl mdl, const wchar_t *name)
{
    int i;
    ProName hn;
    for (i = 0; i < wc->nh; i++) {
        if (mdl && wc->hlist[i] == mdl) return 1;
        if (!mdl && ProMdlMdlnameGet(wc->hlist[i], hn) == PRO_TK_NO_ERROR && _wcsicmp(hn, name) == 0) return 1;
    }
    if (mdl) {
        ProPath org;
        org[0] = 0;
        if (ProMdlOriginGet(mdl, org) == PRO_TK_NO_ERROR && org[0] && file_is_harness_w(org)) return 1;
    }
    return 0;
}

/* ---- supplement: harnesses Toolkit lists for this assembly that the component visit did not report ---- */
typedef struct { ProMdl target; ProFeature found; int ok; } FindCtx;

static ProError find_cb(ProFeature *feat, ProError status, ProAppData data)
{
    FindCtx *fc = (FindCtx *)data;
    ProMdl m = NULL;
    (void)status;
    if (!fc->ok && ProAsmcompMdlGet((ProAsmcomp *)feat, &m) == PRO_TK_NO_ERROR && m == fc->target) {
        fc->found = *feat;
        fc->ok = 1;
    }
    return PRO_TK_NO_ERROR;
}

static int seen_has(WalkCtx *wc, ProMdl h)
{
    int i;
    if (!wc->seen) return 0;
    for (i = 0; i < wc->seen->n; i++) if (wc->seen->h[i] == h) return 1;
    return 0;
}
static void seen_add(WalkCtx *wc, ProMdl h)
{
    if (wc->seen && wc->seen->n < MAX_EMIT && !seen_has(wc, h)) wc->seen->h[wc->seen->n++] = h;
}

/* Report one harness model as a HARNESS component row (no loading). */
static void emit_one_harness(WalkCtx *wc, ProMdl h)
{
    CompInfo ci;
    FindCtx fc;
    ProMdlName hn;
    char base[256];
    ProName featName;

    if (ProMdlMdlnameGet(h, hn) != PRO_TK_NO_ERROR) return;

    memset(&ci, 0, sizeof ci);
    w2u(hn, base, sizeof base);
    _snprintf(ci.file, sizeof ci.file, "%s.prt", base);
    ci.file[sizeof ci.file - 1] = 0;
    lower_ascii(ci.file);
    ci.kind = "HARNESS";
    ci.harness = 1;
    ci.skel = 0;
    ci.mdl = h;
    ci.owner = wc->owner;
    ci.level = wc->level;
    ci.status = "ACTIVE";

    /* the component feature that references this harness (any feature type), if one exists */
    memset(&fc, 0, sizeof fc);
    fc.target = h;
    ProSolidFeatVisit(wc->owner, find_cb, NULL, (ProAppData)&fc);
    if (fc.ok) {
        ci.feat = &fc.found;
        ci.id = fc.found.id;
        if (ProModelitemNameGet((ProModelitem *)&fc.found, featName) == PRO_TK_NO_ERROR)
            w2u(featName, ci.featName, sizeof ci.featName);
        fill_status(&fc.found, &ci);
    } else {
        ci.synthetic = 1;           /* listed by Toolkit's harness list only; no component feature found */
        ci.id = 0;
    }
    memcpy(ci.path, wc->path, sizeof(int) * wc->plen);
    ci.path[wc->plen] = ci.id;
    ci.pathLen = wc->plen + 1;
    wc->cb(&ci, wc->ud);
    if (wc->ne < MAX_EMIT) wc->emitted[wc->ne++] = h;
    seen_add(wc, h);
}

/* harnesses Toolkit lists for THIS assembly that the component visit did not report */
static void emit_missing_harnesses(WalkCtx *wc)
{
    int i, j;
    for (i = 0; i < wc->nh; i++) {
        int seen = 0;
        for (j = 0; j < wc->ne; j++) if (wc->emitted[j] == wc->hlist[i]) { seen = 1; break; }
        if (!seen) emit_one_harness(wc, wc->hlist[i]);
    }
}

/* Root only: harnesses of the assembly AND its sub-assemblies (ProAssemblyHarnessesCollect) that no level reported,
   e.g. a harness whose owner sub-assembly is not expanded. Listed at level 1; no owner path is known. */
static void emit_unlisted_harnesses(WalkCtx *wc)
{
    ProHarness *all = NULL;
    int n = 0, i;
    if (ProAssemblyHarnessesCollect((ProAssembly)wc->owner, &all) != PRO_TK_NO_ERROR || !all) return;
    ProArraySizeGet((ProArray)all, &n);
    for (i = 0; i < n; i++)
        if (!seen_has(wc, (ProMdl)all[i])) emit_one_harness(wc, (ProMdl)all[i]);
    ProArrayFree((ProArray *)&all);
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
        ProMdlfileType ft = PRO_MDLFILE_PART;
        ProMdlType dt;
        mname[0] = 0;
        /* name-only lookups, none of which load the model (harness members often land here) */
        if (ProAsmcompMdlMdlnameGet((ProAsmcomp *)feat, &ft, mname) != PRO_TK_NO_ERROR || mname[0] == 0) {
            mname[0] = 0;
            if (ProAsmcompMdldataGet((ProAsmcomp *)feat, &dt, &ft, mname) != PRO_TK_NO_ERROR || mname[0] == 0) {
                if (wc->skipped) (*wc->skipped)++;
                return PRO_TK_NO_ERROR;           /* no referenced model: not real */
            }
        }
        mtype = (ft == PRO_MDLFILE_ASSEMBLY) ? PRO_MDL_ASSEMBLY : PRO_MDL_PART;
    }

    w2u(mname, base, sizeof base);
    _snprintf(ci.file, sizeof ci.file, "%s%s", base, mtype == PRO_MDL_ASSEMBLY ? ".asm" : ".prt");
    ci.file[sizeof ci.file - 1] = 0;
    lower_ascii(ci.file);

    if (mdl && wc->ne < MAX_EMIT) wc->emitted[wc->ne++] = mdl;

    if (mtype != PRO_MDL_ASSEMBLY && is_harness_component(wc, mdl, mname)) {
        ci.harness = 1;
        ci.skel = 0;
        ci.kind = "HARNESS";
        if (mdl) seen_add(wc, mdl);
        if (!mdl) {      /* name-only harness: remember its handle so the supplement pass does not duplicate it */
            int i;
            ProName hn;
            for (i = 0; i < wc->nh; i++)
                if (ProMdlMdlnameGet(wc->hlist[i], hn) == PRO_TK_NO_ERROR && _wcsicmp(hn, mname) == 0 &&
                    wc->ne < MAX_EMIT)
                    { wc->emitted[wc->ne++] = wc->hlist[i]; seen_add(wc, wc->hlist[i]); }
        }
    } else if (mtype == PRO_MDL_ASSEMBLY) {
        ci.skel = 0;
        ci.kind = "ASSEMBLY";
    } else {
        /* Skeleton detection: ProMdlIsSkeleton on the child when it is in memory; otherwise (name-only
           component) compare with the owner assembly's primary skeleton. Unknown stays -1, never "false". */
        ProBoolean sk = PRO_B_FALSE;
        ci.skel = -1;
        if (mdl && ProMdlIsSkeleton(mdl, &sk) == PRO_TK_NO_ERROR) ci.skel = (sk == PRO_B_TRUE) ? 1 : 0;
        if (ci.skel != 1 && wc->owner) {
            ProMdl sm = NULL;
            ProMdlName sn;
            if (ProAsmSkeletonGet((ProAssembly)wc->owner, &sm) == PRO_TK_NO_ERROR && sm &&
                ProMdlMdlnameGet(sm, sn) == PRO_TK_NO_ERROR && _wcsicmp(mname, sn) == 0)
                ci.skel = 1;
        }
        ci.kind = (ci.skel == 1) ? "SKELETON" : "PART";
    }

    if (ProModelitemNameGet((ProModelitem *)feat, featName) == PRO_TK_NO_ERROR)
        w2u(featName, ci.featName, sizeof ci.featName);
    fill_status(feat, &ci);

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
        WalkCtx *child = (WalkCtx *)malloc(sizeof *child);
        if (child) {
            *child = *wc;
            child->level = wc->level + 1;
            child->plen = ci.pathLen;
            memcpy(child->path, ci.path, sizeof(int) * ci.pathLen);
            child->owner = (ProSolid)mdl;
            load_harnesses(child);
            ProSolidFeatVisit((ProSolid)mdl, w_visit, w_filter, (ProAppData)child);
            emit_missing_harnesses(child);
            free(child);
        }
    }
    return PRO_TK_NO_ERROR;
}

int walk_components(ProSolid root, int recurse, CompCb cb, void *ud, int *skipped)
{
    WalkCtx *wc = (WalkCtx *)calloc(1, sizeof *wc);
    Seen *seen = (Seen *)calloc(1, sizeof *seen);
    ProError err;
    if (!wc || !seen) { free(wc); free(seen); return (int)PRO_TK_OUT_OF_MEMORY; }
    wc->cb = cb; wc->ud = ud; wc->recurse = recurse; wc->level = 1;
    wc->owner = root; wc->skipped = skipped; wc->seen = seen;
    load_harnesses(wc);
    err = ProSolidFeatVisit(root, w_visit, w_filter, (ProAppData)wc);
    if (err == PRO_TK_E_NOT_FOUND) err = PRO_TK_NO_ERROR;   /* no components */
    if (err == PRO_TK_NO_ERROR) {
        emit_missing_harnesses(wc);
        if (recurse) emit_unlisted_harnesses(wc);           /* harnesses of sub-assemblies nobody reported */
    }
    free(seen);
    free(wc);
    return (int)err;
}

/* ---------- /structure ---------- */
typedef struct { Jw *w; int count; int recurse; int unexpanded; } StructOut;

static void structure_cb(const CompInfo *ci, void *ud)
{
    StructOut *so = (StructOut *)ud;
    Jw *w = so->w;
    char p[256];
    int i, n = 0;

    /* Row shape matches Creo.JS Structure rows: id, name (model tip WITH extension), type (PART |
       ASSEMBLY | SKELETON | HARNESS, never COMPONENT), subtype, level, status, suppressed, path. Only real
       components are walked, so datums / CSYS / cross-sections cannot appear here. */
    p[0] = 0;
    for (i = 0; i < ci->pathLen && n < 230; ++i)
        n += sprintf(p + n, i ? "/%d" : "%d", ci->path[i]);

    jw_obj(w, NULL);
    if (ci->synthetic) jw_null(w, "id"); else jw_int(w, "id", ci->id);
    jw_str(w, "name", ci->file);
    jw_str(w, "featureName", ci->featName[0] ? ci->featName : NULL);
    jw_str(w, "type", ci->kind);
    jw_str(w, "subtype", ci->harness ? "HARNESS" : (ci->skel == 1 ? "SKELETON" : ""));
    if (ci->skel < 0) jw_null(w, "is_skeleton"); else jw_bool(w, "is_skeleton", ci->skel == 1);
    jw_bool(w, "is_harness", ci->harness);
    jw_int(w, "level", ci->level);
    jw_str(w, "status", ci->status);
    jw_bool(w, "suppressed", ci->suppressed);
    jw_str(w, "path", p);
    if (so->recurse)
        jw_bool(w, "expanded", ci->mdl != NULL && strcmp(ci->kind, "ASSEMBLY") == 0 && !ci->suppressed);
    jw_end_obj(w);
    if (so->recurse && ci->mdl == NULL && strcmp(ci->kind, "ASSEMBLY") == 0 && !ci->suppressed)
        so->unexpanded++;                     /* sub-assembly not in memory: listed by name only, children not shown */
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
    so.recurse = !(q_get(q, "recurse", v, sizeof v) && v[0] == '0');   /* default: full nested tree; ?recurse=0 = top level only */

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
    jw_int(&w, "unexpandedSubassemblies", so.unexpanded);
    jw_bool(&w, "partial", so.unexpanded > 0);
    jw_raw(&w, "components", comp.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&comp);
}
