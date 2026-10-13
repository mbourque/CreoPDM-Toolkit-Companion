/* model.c - identity, units, parameters, mass, materials */
#include "common.h"
#include <ProMdlUnits.h>
#include <ProParameter.h>
#include <ProParamval.h>
#include <ProModelitem.h>
#include <ProPart.h>
#include <ProMaterial.h>
#include <ProArray.h>
#include <ProAsmcomppath.h>
#include <ProSkeleton.h>

/* ---------- common envelope helpers ---------- */
static void open_ok(Jw *w, Sb *out, const Ctx *c)
{
    jw_init(w, out);
    jw_obj(w, NULL);
    jw_bool(w, "ok", 1);
    jw_str(w, "fileName", c->fileName);
}
static void close_doc(Jw *w, Sb *out)
{
    jw_end_obj(w);
    sb_puts(out, "\n");
}

/* ---------- identity ---------- */
void collect_identity(Sb *out, const Ctx *c, const Query *q)
{
    Jw w;
    ProMdlName nm;
    ProPath origin;
    wchar_t *common = NULL;
    ProBoolean modifiable = PRO_B_FALSE, modified = PRO_B_FALSE;
    ProMdlsubtype st;
    (void)q;

    open_ok(&w, out, c);
    jw_obj(&w, "identity");
    jw_str(&w, "file_name", c->fileName);
    {
        char commonU[400], originU[1100];
        commonU[0] = originU[0] = 0;
        if (ProMdlCommonnameGet(c->mdl, &common, &modifiable) == PRO_TK_NO_ERROR && common) {
            w2u(common, commonU, sizeof commonU);
            ProWstringFree(common);
        }
        origin[0] = 0;
        ProMdlOriginGet(c->mdl, origin);
        w2u(origin, originU, sizeof originU);

        if (c->type == PRO_MDL_MFG) {
            /* For an MFG, Toolkit reports the related top-level ASSEMBLY here. Present the MFG tip as
               the primary identity and keep Toolkit's values under related_assembly. */
            char derived[1200], *slash;
            jw_str(&w, "common_name", c->fileName);
            strncpy(derived, originU, sizeof derived - 1);
            derived[sizeof derived - 1] = 0;
            slash = strrchr(derived, '\\');
            if (!slash) slash = strrchr(derived, '/');
            if (slash && originU[0]) {
                slash[1] = 0;
                strncat(derived, c->fileName, sizeof derived - strlen(derived) - 1);
                jw_str(&w, "origin", derived);          /* folder of Toolkit's origin + MFG file name */
                jw_bool(&w, "origin_derived", 1);       /* version suffix (.N) not available from Toolkit */
            } else {
                jw_null(&w, "origin");
                jw_bool(&w, "origin_derived", 0);
            }
            jw_obj(&w, "related_assembly");
            jw_str(&w, "common_name", commonU);
            jw_str(&w, "origin", originU);
            jw_end_obj(&w);
        } else {
            jw_str(&w, "common_name", commonU);
            jw_str(&w, "origin", originU);
        }
    }
    if (ProMdlMdlnameGet(c->mdl, nm) == PRO_TK_NO_ERROR) jw_wstr(&w, "full_name", nm);
    else jw_str(&w, "full_name", "");
    jw_str(&w, "generic_name", "");     /* not derived: Toolkit has no direct getter here */
    jw_str(&w, "instance_name", "");
    jw_str(&w, "model_type", c->typeStr);
    {
        /* model_role from what Toolkit can tell; "" when it cannot. */
        const char *role = "";
        const char *sub = NULL;
        ProBoolean skel = PRO_B_FALSE;
        if (ProMdlSubtypeGet(c->mdl, &st) == PRO_TK_NO_ERROR) {
            switch (st) {
            case PROMDLSTYPE_PART_SOLID:      sub = "PART_SOLID"; break;
            case PROMDLSTYPE_PART_SHEETMETAL: sub = "PART_SHEETMETAL"; role = "SHEETMETAL"; break;
            case PROMDLSTYPE_PART_COMPOSITE:  sub = "PART_COMPOSITE"; break;
            case PROMDLSTYPE_PART_CONCEPT_MODEL: sub = "PART_CONCEPT"; break;
            case PROMDLSTYPE_ASM_DESIGN:      sub = "ASM_DESIGN"; break;
            case PROMDLSTYPE_ASM_INTERCHANGE: sub = "ASM_INTERCHANGE"; break;
            case PROMDLSTYPE_MFG_NCASM:       sub = "MFG_NCASM"; break;
            case PROMDLSTYPE_NONE:            sub = "NONE"; break;
            default:                          sub = "OTHER"; break;
            }
        }
        if ((c->type == PRO_MDL_PART || c->type == PRO_MDL_ASSEMBLY) &&
            ProMdlIsSkeleton(c->mdl, &skel) == PRO_TK_NO_ERROR && skel == PRO_B_TRUE)
            role = "SKELETON";
        jw_str(&w, "model_role", role);
        jw_str(&w, "subtype", sub);
    }
    if (ProMdlModificationVerify(c->mdl, &modified) == PRO_TK_NO_ERROR)
        jw_bool(&w, "is_modified", modified == PRO_B_TRUE);
    else
        jw_bool(&w, "is_modified", 0);
    jw_null(&w, "version_stamp");
    jw_end_obj(&w);
    close_doc(&w, out);
}

/* ---------- units ---------- */
static void unit_name(ProUnitsystem *us, ProUnitType t, Jw *w, const char *key)
{
    ProUnititem it;
    if (ProUnitsystemUnitGet(us, t, &it) == PRO_TK_NO_ERROR) jw_wstr(w, key, it.name);
    else jw_str(w, key, "");
}

void collect_units(Sb *out, const Ctx *c, const Query *q)
{
    Jw w;
    ProUnitsystem us;
    ProError rc;
    (void)q;
    if (c->type != PRO_MDL_PART && c->type != PRO_MDL_ASSEMBLY && c->type != PRO_MDL_MFG) {
        emit_errorf(out, "units apply to PART/ASSEMBLY/MFG; active model is %s", c->typeStr);
        return;
    }
    rc = ProMdlPrincipalunitsystemGet(c->mdl, &us);
    if (rc != PRO_TK_NO_ERROR) {
        emit_errorf(out, "ProMdlPrincipalunitsystemGet failed (%d)", (int)rc);
        return;
    }
    open_ok(&w, out, c);
    jw_obj(&w, "units");
    jw_wstr(&w, "system_name", us.name);
    unit_name(&us, PRO_UNITTYPE_LENGTH, &w, "length");
    unit_name(&us, PRO_UNITTYPE_MASS, &w, "mass");
    unit_name(&us, PRO_UNITTYPE_TIME, &w, "time");
    unit_name(&us, PRO_UNITTYPE_TEMPERATURE, &w, "temperature");
    unit_name(&us, PRO_UNITTYPE_ANGLE, &w, "angle");
    jw_end_obj(&w);
    close_doc(&w, out);
}

/* ---------- parameters ---------- */
static ProError param_action(ProParameter *p, ProError status, ProAppData data)
{
    Jw *w = (Jw *)data;
    ProParamvalue pv;
    ProUnititem un;
    ProBoolean des = PRO_B_FALSE;
    wchar_t *desc = NULL;
    char val[600];
    const char *dtype = "STRING";
    int have_val = 0;
    (void)status;

    memset(&pv, 0, sizeof pv);
    memset(&un, 0, sizeof un);
    if (ProParameterValueWithUnitsGet(p, &pv, &un) == PRO_TK_NO_ERROR) have_val = 1;
    val[0] = 0;
    if (have_val) {
        switch (pv.type) {
        case PRO_PARAM_DOUBLE:  dtype = "DOUBLE";  sprintf(val, "%.12g", pv.value.d_val); break;
        case PRO_PARAM_INTEGER: dtype = "INTEGER"; sprintf(val, "%d", pv.value.i_val); break;
        case PRO_PARAM_BOOLEAN: dtype = "BOOLEAN"; strcpy(val, pv.value.l_val ? "true" : "false"); break;
        case PRO_PARAM_STRING:  dtype = "STRING";  w2u(pv.value.s_val, val, sizeof val); break;
        case PRO_PARAM_NOTE_ID: dtype = "NOTE_ID"; sprintf(val, "%d", pv.value.i_val); break;
        default:                dtype = "UNKNOWN"; have_val = 0; break;
        }
    }

    jw_obj(w, NULL);
    jw_wstr(w, "name", p->id);
    jw_str(w, "value", have_val ? val : NULL);
    jw_str(w, "data_type", dtype);
    if (have_val && pv.type == PRO_PARAM_DOUBLE && un.name[0]) jw_wstr(w, "units", un.name);
    else jw_null(w, "units");
    if (ProParameterDescriptionGet(p, &desc) == PRO_TK_NO_ERROR && desc) {
        if (desc[0]) jw_wstr(w, "description", desc); else jw_null(w, "description");
        ProWstringFree(desc);
    } else {
        jw_null(w, "description");
    }
    ProParameterDesignationVerify(p, &des);
    jw_bool(w, "is_designated", des == PRO_B_TRUE);
    jw_end_obj(w);
    return PRO_TK_NO_ERROR;
}

void collect_parameters(Sb *out, const Ctx *c, const Query *q)
{
    Jw w;
    ProModelitem mi;
    ProError rc;
    (void)q;
    if (ProMdlToModelitem(c->mdl, &mi) != PRO_TK_NO_ERROR) {
        emit_error(out, "ProMdlToModelitem failed");
        return;
    }
    open_ok(&w, out, c);
    jw_arr(&w, "parameters");
    rc = ProParameterVisit(&mi, NULL, param_action, (ProAppData)&w);
    if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) {
        emit_errorf(out, "ProParameterVisit failed (%d)", (int)rc);
        return;
    }
    jw_end_arr(&w);
    close_doc(&w, out);
}

/* ---------- mass ---------- */
/* Component-sum fallback for assemblies whose native mass call fails.
   Uses only parts already in session; nothing is retrieved. */
typedef struct {
    ProSolid root;
    double   mass, volume, area, mcog[3];
    int      included, no_density, unresolved;
} SumCtx;

static void sum_cb(const CompInfo *ci, void *ud)
{
    SumCtx *s = (SumCtx *)ud;
    ProMassProperty pm;
    ProAsmcomppath cp;
    ProIdTable ids;
    ProMatrix T;
    ProVector out;
    int k;

    if (ci->suppressed) return;
    if (strcmp(ci->kind, "ASSEMBLY") == 0) {
        if (!ci->mdl) s->unresolved++;          /* sub-assembly not in session: cannot descend */
        return;                                  /* leaf parts are visited via recurse */
    }
    if (strcmp(ci->kind, "SKELETON") == 0) return;
    if (!ci->mdl) { s->unresolved++; return; }

    memset(&pm, 0, sizeof pm);
    if (ProSolidMassPropertyGet((ProSolid)ci->mdl, NULL, &pm) != PRO_TK_NO_ERROR) {
        s->unresolved++;
        return;
    }
    if (!(pm.mass > 0.0)) { s->no_density++; return; }   /* contributes 0, as in Creo */

    for (k = 0; k < ci->pathLen; k++) ids[k] = ci->path[k];
    if (ProAsmcomppathInit(s->root, ids, ci->pathLen, &cp) != PRO_TK_NO_ERROR ||
        ProAsmcomppathTrfGet(&cp, PRO_B_TRUE, T) != PRO_TK_NO_ERROR ||
        ProPntTrfEval(pm.center_of_gravity, T, out) != PRO_TK_NO_ERROR) {
        s->unresolved++;
        return;
    }
    s->mass += pm.mass;
    s->volume += pm.volume;
    s->area += pm.surface_area;
    for (k = 0; k < 3; k++) s->mcog[k] += pm.mass * out[k];
    s->included++;
}

void collect_mass(Sb *out, const Ctx *c, const Query *q)
{
    Jw w;
    ProMassProperty mp;
    ProError rc1, rc2 = PRO_TK_NO_ERROR, rc3 = PRO_TK_NO_ERROR;
    const char *method = "ProSolidMassPropertyGet";
    int have = 0, has_moments = 1, i, j;
    SumCtx sc;
    char diag[400];
    (void)q;

    if (c->type != PRO_MDL_PART && c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "mass applies to PART/ASSEMBLY; active model is %s", c->typeStr);
        return;
    }
    memset(&mp, 0, sizeof mp);
    rc1 = ProSolidMassPropertyGet((ProSolid)c->mdl, NULL, &mp);   /* default csys, model units */
    have = (rc1 == PRO_TK_NO_ERROR);

    if (!have && c->type == PRO_MDL_ASSEMBLY) {
        memset(&mp, 0, sizeof mp);
        rc2 = ProAssemblySolidMassPropertyGet((ProSolid)c->mdl, NULL, &mp);
        if (rc2 == PRO_TK_NO_ERROR) { have = 1; method = "ProAssemblySolidMassPropertyGet"; }
    }
    if (!have && c->type == PRO_MDL_ASSEMBLY) {
        memset(&mp, 0, sizeof mp);
        rc3 = ProSolidMassPropertyWithDensityGet((ProSolid)c->mdl, NULL, PRO_MP_DENS_DEFAULT, 0.0, &mp);
        if (rc3 == PRO_TK_NO_ERROR) { have = 1; method = "ProSolidMassPropertyWithDensityGet"; }
    }

    if (!have && c->type == PRO_MDL_ASSEMBLY) {
        int skipped = 0, k;
        memset(&sc, 0, sizeof sc);
        sc.root = (ProSolid)c->mdl;
        walk_components(sc.root, 1, sum_cb, &sc, &skipped);
        sprintf(diag, "ProSolidMassPropertyGet=%d, ProAssemblySolidMassPropertyGet=%d, "
                      "ProSolidMassPropertyWithDensityGet=%d; component sum: %d part(s) with mass, "
                      "%d without density, %d unresolved/not in session",
                (int)rc1, (int)rc2, (int)rc3, sc.included, sc.no_density, sc.unresolved + skipped);
        if (sc.included > 0 && sc.unresolved == 0 && skipped == 0 && sc.mass > 0.0) {
            memset(&mp, 0, sizeof mp);
            mp.mass = sc.mass;
            mp.volume = sc.volume;
            mp.surface_area = sc.area;
            mp.density = sc.volume > 0.0 ? sc.mass / sc.volume : 0.0;
            for (k = 0; k < 3; k++) mp.center_of_gravity[k] = sc.mcog[k] / sc.mass;
            have = 1;
            has_moments = 0;                 /* inertia not derived from the sum */
            method = "component_sum";
        } else {
            emit_errorf(out, "assembly mass unavailable (%s)", diag);
            return;
        }
    } else if (!have) {
        emit_errorf(out, "ProSolidMassPropertyGet failed (%d)", (int)rc1);
        return;
    }

    if (!(mp.mass > 0.0) || !(mp.density > 0.0)) {
        /* Never invent zeros: report why, keep the geometric value we do have. */
        Jw e;
        out->len = 0; if (out->p) out->p[0] = 0;
        jw_init(&e, out);
        jw_obj(&e, NULL);
        jw_bool(&e, "ok", 0);
        jw_str(&e, "fileName", c->fileName);
        jw_str(&e, "error", c->type == PRO_MDL_PART
               ? "Mass unavailable: no material/density assigned to the part"
               : "Mass unavailable: no component has a density (assign materials)");
        if (mp.volume > 0.0) jw_dbl(&e, "volume", mp.volume);
        jw_end_obj(&e);
        sb_puts(out, "\n");
        return;
    }

    open_ok(&w, out, c);
    jw_obj(&w, "mass");
    jw_dbl(&w, "mass", mp.mass);
    jw_dbl(&w, "volume", mp.volume);
    jw_dbl(&w, "surface_area", mp.surface_area);
    jw_dbl(&w, "density", mp.density);
    jw_arr(&w, "gravity_center");
    for (i = 0; i < 3; i++) jw_dbl(&w, NULL, mp.center_of_gravity[i]);
    jw_end_arr(&w);
    if (has_moments) {
        jw_arr(&w, "principal_moments");
        for (i = 0; i < 3; i++) jw_dbl(&w, NULL, mp.principal_moments[i]);
        jw_end_arr(&w);
        jw_arr(&w, "cg_inertia_tensor");        /* extra: about CG, 3x3 row-major */
        for (i = 0; i < 3; i++) {
            jw_arr(&w, NULL);
            for (j = 0; j < 3; j++) jw_dbl(&w, NULL, mp.cg_inertia_tensor[i][j]);
            jw_end_arr(&w);
        }
        jw_end_arr(&w);
    } else {
        jw_null(&w, "principal_moments");
    }
    jw_end_obj(&w);
    jw_str(&w, "method", method);
    close_doc(&w, out);
}

/* ---------- materials ---------- */
void collect_materials(Sb *out, const Ctx *c, const Query *q)
{
    Jw w;
    ProName cur;
    ProName *names = NULL;
    int n = 0, i, found = 0;
    char curU[128];
    (void)q;

    if (c->type != PRO_MDL_PART) {
        emit_errorf(out, "materials are per PART; active model is %s", c->typeStr);
        return;
    }
    cur[0] = 0;
    ProPartMaterialNameGet((ProPart)c->mdl, cur);
    curU[0] = 0;
    w2u(cur, curU, sizeof curU);

    if (ProArrayAlloc(0, sizeof(ProName), 1, (ProArray *)&names) != PRO_TK_NO_ERROR) {
        emit_error(out, "ProArrayAlloc failed");
        return;
    }
    if (ProPartMaterialsGet((ProSolid)c->mdl, &names) == PRO_TK_NO_ERROR)
        ProArraySizeGet((ProArray)names, &n);
    else
        n = 0;

    open_ok(&w, out, c);
    jw_obj(&w, "materials");
    if (curU[0]) jw_str(&w, "current", curU); else jw_null(&w, "current");
    jw_arr(&w, "names");
    for (i = 0; i < n; i++) {
        jw_wstr(&w, NULL, names[i]);
        if (curU[0] && wcscmp(names[i], cur) == 0) found = 1;
    }
    if (curU[0] && !found) jw_str(&w, NULL, curU);
    jw_end_arr(&w);
    jw_end_obj(&w);
    close_doc(&w, out);
    ProArrayFree((ProArray *)&names);
}

