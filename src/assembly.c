/* assembly.c - /constraints and /simp-reps */
#include "common.h"
#include <ProMenuBar.h>
#include <ProFeature.h>
#include <ProModelitem.h>
#include <ProAsmcomp.h>
#include <ProAsmcomppath.h>
#include <ProSelection.h>
#include <ProSimprep.h>
#include <ProSimprepdata.h>
#include <ProArray.h>
#include <ProMdlUnits.h>

/* ---------- constraints ---------- */
static const char *constr_type(ProAsmcompConstrType t)
{
    switch (t) {
    case PRO_ASM_MATE:         return "MATE";
    case PRO_ASM_MATE_OFF:     return "MATE_OFFSET";
    case PRO_ASM_ALIGN:        return "ALIGN";
    case PRO_ASM_ALIGN_OFF:    return "ALIGN_OFFSET";
    case PRO_ASM_INSERT:       return "INSERT";
    case PRO_ASM_ORIENT:       return "ORIENT";
    case PRO_ASM_CSYS:         return "CSYS";
    case PRO_ASM_TANGENT:      return "TANGENT";
    case PRO_ASM_PNT_ON_SRF:   return "PNT_ON_SRF";
    case PRO_ASM_EDGE_ON_SRF:  return "EDGE_ON_SRF";
    case PRO_ASM_DEF_PLACEMENT:return "DEFAULT";            /* Default placement */
    case PRO_ASM_SUBSTITUTE:   return "SUBSTITUTE";
    case PRO_ASM_PNT_ON_LINE:  return "PNT_ON_LINE";
    case PRO_ASM_FIX:          return "FIX";                /* Fixed in place */
    case PRO_ASM_AUTO:         return "AUTO";
    case PRO_ASM_ALIGN_ANG_OFF:return "ALIGN_ANGLE_OFFSET";
    case PRO_ASM_MATE_ANG_OFF: return "MATE_ANGLE_OFFSET";
    case PRO_ASM_CSYS_PNT:     return "CSYS_PNT";
    case PRO_ASM_LINE_NORMAL:  return "LINE_NORMAL";
    case PRO_ASM_LINE_COPLANAR:return "LINE_COPLANAR";
    case PRO_ASM_LINE_PARL:    return "LINE_PARALLEL";
    case PRO_ASM_LINE_DIST:    return "LINE_DISTANCE";
    case PRO_ASM_PNT_DIST:     return "PNT_DISTANCE";
    case PRO_ASM_INSERT_NORM:  return "INSERT_NORMAL";
    case PRO_ASM_INSERT_PARL:  return "INSERT_PARALLEL";
    case PRO_ASM_PNT_ON_LINE_DIST:    return "PNT_ON_LINE_DISTANCE";
    case PRO_ASM_PNT_ON_SRF_DIST:     return "PNT_ON_SRF_DISTANCE";
    case PRO_ASM_EDGE_ON_SRF_DIST:    return "EDGE_ON_SRF_DISTANCE";
    case PRO_ASM_EDGE_ON_SRF_ANG:     return "EDGE_ON_SRF_ANGLE";
    case PRO_ASM_EDGE_ON_SRF_NORMAL:  return "EDGE_ON_SRF_NORMAL";
    case PRO_ASM_ALIGN_NODEP_ANGLE:   return "ALIGN_ANGLE";
    case PRO_ASM_MATE_NODEP_ANGLE:    return "MATE_ANGLE";
    case PRO_ASM_LINE_ANGLE:          return "LINE_ANGLE";
    case PRO_ASM_EDGE_ON_SRF_PARL:    return "EDGE_ON_SRF_PARALLEL";
    case PRO_ASM_SRF_NORMAL:          return "SRF_NORMAL";
    case PRO_ASM_EXPLICIT:            return "EXPLICIT";
    case 36:                          return "TANGENT_ALIGN";   /* PRO_ASM_TANGENT_ALIGN; matched by value: the enum name is missing in older SDKs */
    case PRO_ASM_UNDEF:        return "UNDEFINED";
    default:                   return "UNKNOWN";
    }
}
/* 0 = no offset, 1 = linear offset, 2 = angular offset */
static int offset_kind(ProAsmcompConstrType t)
{
    switch (t) {
    case PRO_ASM_MATE_OFF: case PRO_ASM_ALIGN_OFF:
    case PRO_ASM_LINE_DIST: case PRO_ASM_PNT_DIST:
    case PRO_ASM_PNT_ON_LINE_DIST: case PRO_ASM_PNT_ON_SRF_DIST:
    case PRO_ASM_EDGE_ON_SRF_DIST:
        return 1;
    case PRO_ASM_ALIGN_ANG_OFF: case PRO_ASM_MATE_ANG_OFF:
    case PRO_ASM_EDGE_ON_SRF_ANG: case PRO_ASM_ALIGN_NODEP_ANGLE:
    case PRO_ASM_MATE_NODEP_ANGLE: case PRO_ASM_LINE_ANGLE:
        return 2;
    default:
        return 0;
    }
}
/* Length unit name of a solid (e.g. "mm"); "" when unknown. */
static void length_unit(ProSolid s, char *buf, int n)
{
    ProUnitsystem us;
    ProUnititem it;
    buf[0] = 0;
    if (ProMdlPrincipalunitsystemGet((ProMdl)s, &us) == PRO_TK_NO_ERROR &&
        ProUnitsystemUnitGet(&us, PRO_UNITTYPE_LENGTH, &it) == PRO_TK_NO_ERROR)
        w2u(it.name, buf, n);
}

static const char *item_type_str(int t)
{
    switch (t) {
    case PRO_SURFACE: return "SURFACE";
    case PRO_EDGE:    return "EDGE";
    case PRO_AXIS:    return "AXIS";
    case PRO_POINT:   return "POINT";
    case PRO_CSYS:    return "CSYS";
    case PRO_FEATURE: return "FEATURE";
    case PRO_CURVE:   return "CURVE";
    default:          return "OTHER";
    }
}

/* One selection reference: {item_type, id, name, path:[ids]} or null */
static void write_ref(Jw *w, const char *key, ProSelection sel)
{
    ProModelitem mi;
    ProAsmcomppath cp;
    ProName nm;
    int i;
    if (!sel || ProSelectionModelitemGet(sel, &mi) != PRO_TK_NO_ERROR) { jw_null(w, key); return; }
    jw_obj(w, key);
    jw_str(w, "item_type", item_type_str((int)mi.type));
    jw_int(w, "id", mi.id);
    if (ProModelitemNameGet(&mi, nm) == PRO_TK_NO_ERROR && nm[0]) jw_wstr(w, "name", nm);
    else jw_null(w, "name");
    jw_arr(w, "path");
    if (ProSelectionAsmcomppathGet(sel, &cp) == PRO_TK_NO_ERROR)
        for (i = 0; i < cp.table_num; i++) jw_int(w, NULL, cp.comp_id_table[i]);
    jw_end_arr(w);
    jw_end_obj(w);
}

typedef struct { Jw *w; ProSolid root; int errors; Jw *ew; } ConsCtx;

static void cons_cb(const CompInfo *ci, void *ud)
{
    ConsCtx *cc = (ConsCtx *)ud;
    Jw *w = cc->w;

    if (!ci->feat) return;                     /* harness listed only by Toolkit's harness list: no component feature */
    ProAsmcompconstraint *arr = NULL;
    ProAsmcomppath owner;
    ProAsmcomppath *ownerp = NULL;
    ProError rc;
    int n = 0, i;
    char p[256]; int k, np = 0;
    char lunit[32];
    ProBoolean packaged = PRO_B_FALSE;

    if (ci->level > 1) {            /* path to the owning sub-assembly */
        ProIdTable ids;
        for (k = 0; k < ci->pathLen - 1; k++) ids[k] = ci->path[k];
        if (ProAsmcomppathInit(cc->root, ids, ci->pathLen - 1, &owner) == PRO_TK_NO_ERROR)
            ownerp = &owner;
    }
    rc = ProAsmcompConstraintsWithComppathGet(ci->feat, ownerp, &arr);
    length_unit(ci->owner, lunit, sizeof lunit);
    ProAsmcompIsPackaged(ci->feat, &packaged);

    p[0] = 0;
    for (k = 0; k < ci->pathLen && np < 230; k++) np += sprintf(p + np, k ? "/%d" : "%d", ci->path[k]);

    jw_obj(w, NULL);
    jw_int(w, "id", ci->id);
    jw_str(w, "name", ci->file);
    jw_str(w, "path", p);
    jw_int(w, "level", ci->level);
    jw_bool(w, "suppressed", ci->suppressed);
    jw_bool(w, "packaged", packaged == PRO_B_TRUE);
    jw_arr(w, "constraints");
    if (rc == PRO_TK_NO_ERROR && arr) {
        ProArraySizeGet((ProArray)arr, &n);
        for (i = 0; i < n; i++) {
            ProAsmcompConstrType t = PRO_ASM_UNDEF;
            ProSelection asel = NULL, csel = NULL;
            ProDatumside aside = PRO_DATUM_SIDE_YELLOW, cside = PRO_DATUM_SIDE_YELLOW;
            double off = 0.0;
            int has_off, ok_type, kind;
            ok_type = (ProAsmcompconstraintTypeGet(arr[i], &t) == PRO_TK_NO_ERROR);
            ProAsmcompconstraintAsmreferenceGet(arr[i], &asel, &aside);
            ProAsmcompconstraintCompreferenceGet(arr[i], &csel, &cside);
            kind = offset_kind(t);
            has_off = kind && ProAsmcompconstraintOffsetGet(arr[i], &off) == PRO_TK_NO_ERROR;
            jw_obj(w, NULL);
            jw_str(w, "type", ok_type ? constr_type(t) : "UNKNOWN");
            jw_int(w, "type_id", (int)t);
            if (has_off) jw_dbl(w, "offset", off); else jw_null(w, "offset");
            if (has_off) jw_str(w, "offset_units", kind == 2 ? "deg" : (lunit[0] ? lunit : NULL));
            else jw_null(w, "offset_units");
            write_ref(w, "assembly_reference", asel);
            write_ref(w, "component_reference", csel);
            jw_int(w, "assembly_datum_side", (int)aside);
            jw_int(w, "component_datum_side", (int)cside);
            jw_end_obj(w);
        }
    } else if (rc != PRO_TK_E_NOT_FOUND && rc != PRO_TK_NO_ERROR) {
        cc->errors++;
        jw_str(cc->ew, NULL, "constraints unavailable for a component");
    }
    jw_end_arr(w);
    jw_end_obj(w);
    if (arr) ProAsmcompconstraintArrayFree(arr), ProArrayFree((ProArray *)&arr);
}

void collect_constraints(Sb *out, const Ctx *c, const Query *q)
{
    Sb comps = { 0 }, errs = { 0 };
    Jw cw, ew, w;
    ConsCtx cc;
    char v[8];
    int skipped = 0, rc, recurse;

    if (c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "constraints apply to ASSEMBLY; active model is %s%s", c->typeStr, c->type == PRO_MDL_MFG ? " (use /mfg)" : "");
        return;
    }
    recurse = (q_get(q, "recurse", v, sizeof v) && v[0] == '1');
    jw_init(&cw, &comps); jw_arr(&cw, NULL);
    jw_init(&ew, &errs);  jw_arr(&ew, NULL);
    cc.w = &cw; cc.ew = &ew; cc.root = (ProSolid)c->mdl; cc.errors = 0;
    rc = walk_components((ProSolid)c->mdl, recurse, cons_cb, &cc, &skipped);
    jw_end_arr(&cw); jw_end_arr(&ew);
    if (rc != PRO_TK_NO_ERROR) {
        emit_errorf(out, "component walk failed (%d)", rc);
        sb_free(&comps); sb_free(&errs);
        return;
    }
    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_bool(&w, "recurse", recurse);
    jw_raw(&w, "components", comps.p);
    jw_raw(&w, "errors", errs.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&comps); sb_free(&errs);
}

/* ---------- simplified representations ---------- */
typedef struct { Jw *w; int active_id; wchar_t active_name[PRO_NAME_SIZE]; int count; } SimpCtx;

static ProError simp_visit(ProGeomitem *item, ProError status, ProAppData data)
{
    SimpCtx *sc = (SimpCtx *)data;
    ProSimprep rep;
    ProSimprepdata *sd = NULL;
    ProName nm;
    (void)status;
    memcpy(&rep, item, sizeof rep);
    nm[0] = 0;
    if (ProSimprepdataGet(&rep, &sd) == PRO_TK_NO_ERROR && sd) {
        ProSimprepdataNameGet(sd, nm);
        ProSimprepdataFree(&sd);
    }
    jw_obj(sc->w, NULL);
    /* Non-positive ids are Toolkit sentinels (e.g. the default rep): report null, never the raw value. */
    if (item->id > 0) jw_int(sc->w, "id", item->id); else jw_null(sc->w, "id");
    jw_wstr(sc->w, "name", nm);
    jw_bool(sc->w, "is_active",
            (item->id > 0 && item->id == sc->active_id) ||
            (sc->active_name[0] && nm[0] && wcscmp(nm, sc->active_name) == 0));
    jw_end_obj(sc->w);
    sc->count++;
    return PRO_TK_NO_ERROR;
}

void collect_simpreps(Sb *out, const Ctx *c, const Query *q)
{
    Sb reps = { 0 };
    Jw rw, w;
    SimpCtx sc;
    ProSimprep active;
    ProSimprepdata *ad = NULL;
    ProName an;
    int is_master = 1, aid = -1;
    (void)q;

    if (c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "simp-reps apply to ASSEMBLY; active model is %s", c->typeStr);
        return;
    }
    an[0] = 0;
    memset(&active, 0, sizeof active);
    if (ProSimprepActiveGet((ProSolid)c->mdl, &active) == PRO_TK_NO_ERROR) {
        /* Ids can be sentinels (e.g. default rep): trust the name, use the id only if positive. */
        if (ProSimprepdataGet(&active, &ad) == PRO_TK_NO_ERROR && ad) {
            ProSimprepdataNameGet(ad, an);
            ProSimprepdataFree(&ad);
        }
        if (active.id > 0) aid = active.id;
        if (an[0]) {
            wchar_t up[PRO_NAME_SIZE];
            wcsncpy(up, an, PRO_NAME_SIZE - 1);
            up[PRO_NAME_SIZE - 1] = 0;
            _wcsupr(up);
            is_master = (wcsstr(up, L"MASTER") != NULL);
        } else {
            is_master = 1;
        }
    }
    jw_init(&rw, &reps);
    jw_arr(&rw, NULL);
    memset(&sc, 0, sizeof sc);
    sc.w = &rw; sc.active_id = aid;
    if (!is_master) wcsncpy(sc.active_name, an, PRO_NAME_SIZE - 1);
    ProSolidSimprepVisit((ProSolid)c->mdl, NULL, simp_visit, (ProAppData)&sc);
    jw_end_arr(&rw);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_obj(&w, "active");
    if (is_master) { jw_null(&w, "id"); jw_str(&w, "name", "MASTER"); jw_bool(&w, "is_master", 1); }
    else           { if (aid > 0) jw_int(&w, "id", aid); else jw_null(&w, "id");
                     jw_wstr(&w, "name", an); jw_bool(&w, "is_master", 0); }
    jw_end_obj(&w);
    jw_raw(&w, "representations", reps.p);
    jw_raw(&w, "errors", "[]");
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&reps);
}
