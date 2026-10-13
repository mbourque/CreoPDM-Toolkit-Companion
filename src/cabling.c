/* cabling.c - /harness-probe: diagnostic dump of what Toolkit knows about harnesses in an assembly.
 * Session data only; nothing is retrieved. Use it to find out how a given assembly represents its harness
 * (component part, GROUP_HEAD members, cable features, spools) when /structure or /bom does not list it. */
#include "common.h"
#include <ProMenuBar.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProAsmcomp.h>
#include <ProCabling.h>
#include <ProUdf.h>
#include <ProGroup.h>
#include <ProArray.h>

#define PROBE_MAX 200

static void write_handle_names(Jw *w, const char *key, ProHarness *arr)
{
    int n = 0, i;
    jw_arr(w, key);
    if (arr) {
        ProArraySizeGet((ProArray)arr, &n);
        for (i = 0; i < n && i < PROBE_MAX; i++) {
            ProMdlName nm;
            if (ProMdlMdlnameGet((ProMdl)arr[i], nm) == PRO_TK_NO_ERROR) jw_wstr(w, NULL, nm);
            else jw_null(w, NULL);
        }
    }
    jw_end_arr(w);
}

static int is_cabling_type(ProFeattype ft)
{
    return ft == PRO_FEAT_CABLE || ft == PRO_FEAT_CABLE_SEGM || ft == PRO_FEAT_SUBHARNESS ||
           ft == PRO_FEAT_RIBBON_CABLE || ft == PRO_FEAT_CABLE_COSM;
}

typedef struct { Jw *gw; Jw *cw; ProSolid asm_mdl; int groups, cabling; } PCtx;

static void feat_brief(Jw *w, ProFeature *f)
{
    ProName tn, nm;
    ProFeattype ft = 0;
    char type[96];
    ProMdl m = NULL;
    ProMdlfileType mt;
    ProFamilyMdlName mn;

    ProFeatureTypeGet(f, &ft);
    type[0] = 0;
    if (ProFeatureTypenameGet(f, tn) == PRO_TK_NO_ERROR) w2u(tn, type, sizeof type);
    jw_obj(w, NULL);
    jw_int(w, "id", f->id);
    jw_int(w, "type_id", (int)ft);
    jw_str(w, "type", type);
    if (ProModelitemNameGet((ProModelitem *)f, nm) == PRO_TK_NO_ERROR) jw_wstr(w, "name", nm); else jw_null(w, "name");
    if (ft == PRO_FEAT_COMPONENT) {
        mn[0] = 0;
        jw_bool(w, "in_memory", ProAsmcompMdlGet((ProAsmcomp *)f, &m) == PRO_TK_NO_ERROR && m != NULL);
        if (ProAsmcompMdlMdlnameGet((ProAsmcomp *)f, &mt, mn) == PRO_TK_NO_ERROR && mn[0]) jw_wstr(w, "model", mn);
        else jw_null(w, "model");
    }
    jw_end_obj(w);
}

static ProError probe_visit(ProFeature *f, ProError status, ProAppData data)
{
    PCtx *pc = (PCtx *)data;
    ProFeattype ft = 0;
    (void)status;
    ProFeatureTypeGet(f, &ft);

    if (ft == PRO_FEAT_GROUP_HEAD && pc->groups < PROBE_MAX) {
        ProGroup g;
        ProFeature *members = NULL;
        ProName nm;
        int n = 0, i;
        memcpy(&g, f, sizeof g);                       /* a group is addressed by its head feature */
        jw_obj(pc->gw, NULL);
        jw_int(pc->gw, "id", f->id);
        if (ProModelitemNameGet((ProModelitem *)f, nm) == PRO_TK_NO_ERROR) jw_wstr(pc->gw, "name", nm);
        else jw_null(pc->gw, "name");
        jw_arr(pc->gw, "members");
        if (ProGroupFeaturesCollect(&g, &members) == PRO_TK_NO_ERROR && members) {
            ProArraySizeGet((ProArray)members, &n);
            for (i = 0; i < n && i < PROBE_MAX; i++) feat_brief(pc->gw, &members[i]);
            ProArrayFree((ProArray *)&members);
        }
        jw_end_arr(pc->gw);
        jw_end_obj(pc->gw);
        pc->groups++;
    }
    if (is_cabling_type(ft) && pc->cabling < PROBE_MAX) {
        feat_brief(pc->cw, f);
        pc->cabling++;
    }
    return PRO_TK_NO_ERROR;
}

void collect_harness_probe(Sb *out, const Ctx *c, const Query *q)
{
    Sb groups = { 0 }, cabling = { 0 };
    Jw gw, cw, w;
    PCtx pc;
    ProHarness *top = NULL, *all = NULL;
    ProSpool *spools = NULL;
    ProConnector *cons = NULL;
    ProError rtop, rall, rspool, rcon;
    int n;
    (void)q;

    if (c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "harness probe applies to ASSEMBLY; active model is %s", c->typeStr);
        return;
    }
    rtop = ProAssemblyHarnessesTopCollect((ProAssembly)c->mdl, &top);
    rall = ProAssemblyHarnessesCollect((ProAssembly)c->mdl, &all);
    rspool = ProAssemblySpoolsCollect((ProAssembly)c->mdl, &spools);
    rcon = ProAssemblyConnectorsGet((ProAssembly)c->mdl, &cons);

    jw_init(&gw, &groups); jw_arr(&gw, NULL);
    jw_init(&cw, &cabling); jw_arr(&cw, NULL);
    memset(&pc, 0, sizeof pc);
    pc.gw = &gw; pc.cw = &cw; pc.asm_mdl = (ProSolid)c->mdl;
    ProSolidFeatVisit((ProSolid)c->mdl, probe_visit, NULL, (ProAppData)&pc);
    jw_end_arr(&gw); jw_end_arr(&cw);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_obj(&w, "probe");
    jw_int(&w, "ProAssemblyHarnessesTopCollect_rc", (int)rtop);
    write_handle_names(&w, "harnessesTop", rtop == PRO_TK_NO_ERROR ? top : NULL);
    jw_int(&w, "ProAssemblyHarnessesCollect_rc", (int)rall);
    write_handle_names(&w, "harnessesAll", rall == PRO_TK_NO_ERROR ? all : NULL);
    jw_int(&w, "ProAssemblySpoolsCollect_rc", (int)rspool);
    jw_arr(&w, "spools");
    if (rspool == PRO_TK_NO_ERROR && spools) {
        ProArraySizeGet((ProArray)spools, &n);
        { int i; for (i = 0; i < n && i < PROBE_MAX; i++) jw_wstr(&w, NULL, spools[i].name); }
    }
    jw_end_arr(&w);
    jw_int(&w, "ProAssemblyConnectorsGet_rc", (int)rcon);
    n = 0;
    if (rcon == PRO_TK_NO_ERROR && cons) ProArraySizeGet((ProArray)cons, &n);
    jw_int(&w, "connectorCount", n);
    jw_raw(&w, "groups", groups.p);
    jw_raw(&w, "cablingFeatures", cabling.p);
    jw_end_obj(&w);
    jw_end_obj(&w);
    sb_puts(out, "\n");

    if (top) ProArrayFree((ProArray *)&top);
    if (all) ProArrayFree((ProArray *)&all);
    if (spools) ProArrayFree((ProArray *)&spools);
    if (cons) ProArrayFree((ProArray *)&cons);
    sb_free(&groups);
    sb_free(&cabling);
}
