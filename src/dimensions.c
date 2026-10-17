/* dimensions.c - /dimensions: the model (feature) dimensions of a part or assembly.
 *
 * One row per model dimension, NOT drawing annotations: {symbol, value, units, feature_id, feature_name, ...}.
 * feature_id is the id /features uses (the feature that DIRECTLY owns the dimension, ProDimensionOwnerfeatureGet), so
 * a consumer can attach dimensions to feature rows. Dimensions of features that /features does not list are left out
 * (internal/invisible features, "no_name" placeholders, component features), so every row maps to a listed feature.
 * Reference dimensions are skipped unless ?reference=1.
 * Units: the model's length unit (ANGLE -> "deg"). Toolkit has no per-dimension unit call, so they come from the model;
 * a dimension whose type could not be read is given the length unit with units_assumed:true. */
#include "common.h"
#include <ProMenuBar.h>
#include <ProDimension.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProMdlUnits.h>

typedef struct {
    Jw   *w;
    char  lunit[40];
    int   reference;
    int   count;
} DimCtx;

static const char *dim_type_label(ProDimensiontype t)
{
    switch (t) {
    case PRODIMTYPE_LINEAR:     return "LINEAR";
    case PRODIMTYPE_RADIUS:     return "RADIUS";
    case PRODIMTYPE_DIAMETER:   return "DIAMETER";
    case PRODIMTYPE_ANGLE:      return "ANGLE";
    case PRODIMTYPE_ARC_LENGTH: return "ARC_LENGTH";
    default:                    return "OTHER";
    }
}

static ProError dim_row(ProDimension *dim, ProError status, ProAppData data)
{
    DimCtx *dc = (DimCtx *)data;
    Jw *w = dc->w;
    ProFeature feat;
    ProFeattype ft = 0;
    ProBoolean vis = PRO_B_TRUE;
    ProName sym, nm, tn;
    ProDimensiontype dt = PRODIMTYPE_UNKNOWN;
    ProError trc;
    char symU[64], fname[160], ftype[96];
    double val = 0.0, shown = 0.0;
    int has_val, has_shown, has_feat, type_known, id = 0;
    const char *units;
    (void)status;

    symU[0] = fname[0] = ftype[0] = 0;
    has_feat = (ProDimensionOwnerfeatureGet(dim, &feat) == PRO_TK_NO_ERROR);
    if (has_feat) {
        ProFeatureTypeGet(&feat, &ft);
        if (ft == PRO_FEAT_COMPONENT) return PRO_TK_NO_ERROR;                 /* component placement is not a feature dimension */
        if (ProFeatureVisibilityGet(&feat, &vis) == PRO_TK_NO_ERROR && vis == PRO_B_FALSE) return PRO_TK_NO_ERROR;
        id = feat.id;
        if (ProModelitemNameGet((ProModelitem *)&feat, nm) == PRO_TK_NO_ERROR) w2u(nm, fname, sizeof fname);
        if (is_noname_u(fname)) return PRO_TK_NO_ERROR;                       /* hidden unnamed feature: not listed by /features */
        if (ProFeatureTypenameGet(&feat, tn) == PRO_TK_NO_ERROR) {
            char *t;
            w2u(tn, ftype, sizeof ftype);
            for (t = ftype; *t; ++t) *t = (*t == ' ') ? '_' : (*t >= 'a' && *t <= 'z') ? (char)(*t - 32) : *t;   /* "CHAMFER" */
        }
    }

    if (ProDimensionSymbolGet(dim, sym) == PRO_TK_NO_ERROR) w2u(sym, symU, sizeof symU);
    has_val = (ProDimensionValueGet(dim, &val) == PRO_TK_NO_ERROR);
    has_shown = (ProDimensionDisplayedValueGet(dim, &shown) == PRO_TK_NO_ERROR);
    trc = ProDimensionTypeGet(dim, &dt);
    type_known = (trc == PRO_TK_NO_ERROR) &&
                 (dt == PRODIMTYPE_LINEAR || dt == PRODIMTYPE_RADIUS || dt == PRODIMTYPE_DIAMETER ||
                  dt == PRODIMTYPE_ANGLE || dt == PRODIMTYPE_ARC_LENGTH);
    units = (type_known && dt == PRODIMTYPE_ANGLE) ? "deg" : (dc->lunit[0] ? dc->lunit : NULL);

    jw_obj(w, NULL);
    jw_str(w, "symbol", symU[0] ? symU : NULL);
    if (has_val) jw_dbl(w, "value", val); else jw_null(w, "value");
    if (has_shown) jw_dbl(w, "value_displayed", shown); else jw_null(w, "value_displayed");
    jw_str(w, "units", units);
    jw_str(w, "type", type_known ? dim_type_label(dt) : (trc == PRO_TK_NO_ERROR ? "OTHER" : "UNKNOWN"));
    if (!type_known) jw_bool(w, "units_assumed", 1);
    if (has_feat) jw_int(w, "feature_id", id); else jw_null(w, "feature_id");
    jw_str(w, "feature_name", fname[0] ? fname : (ftype[0] ? ftype : NULL));     /* user name if the feature has one, else its type name */
    jw_str(w, "feature_type", ftype[0] ? ftype : NULL);
    if (dc->reference) jw_bool(w, "reference", 1);
    jw_end_obj(w);
    dc->count++;
    return PRO_TK_NO_ERROR;
}

void collect_dimensions(Sb *out, const Ctx *c, const Query *q)
{
    Sb body = { 0 };
    Jw bw, w;
    DimCtx dc;
    ProUnitsystem us;
    ProUnititem it;
    char v[8];
    int with_ref;
    ProError rc;

    if (c->type != PRO_MDL_PART && c->type != PRO_MDL_ASSEMBLY) {
        emit_errorf(out, "dimensions apply to PART/ASSEMBLY; active model is %s%s", c->typeStr,
                    c->type == PRO_MDL_DRAWING ? " (use /drawing for drawing dimensions)" : "");
        return;
    }
    with_ref = (q_get(q, "reference", v, sizeof v) && v[0] == '1');

    memset(&dc, 0, sizeof dc);
    if (ProMdlPrincipalunitsystemGet(c->mdl, &us) == PRO_TK_NO_ERROR &&
        ProUnitsystemUnitGet(&us, PRO_UNITTYPE_LENGTH, &it) == PRO_TK_NO_ERROR)
        w2u(it.name, dc.lunit, sizeof dc.lunit);

    jw_init(&bw, &body);
    jw_arr(&bw, NULL);
    dc.w = &bw;
    rc = ProSolidDimensionVisit((ProSolid)c->mdl, PRO_B_FALSE, dim_row, NULL, (ProAppData)&dc);   /* driving dimensions */
    if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) {
        emit_errorf(out, "ProSolidDimensionVisit failed (%d)", (int)rc);
        sb_free(&body);
        return;
    }
    if (with_ref) {
        dc.reference = 1;
        ProSolidDimensionVisit((ProSolid)c->mdl, PRO_B_TRUE, dim_row, NULL, (ProAppData)&dc);
    }
    jw_end_arr(&bw);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_int(&w, "dimensionCount", dc.count);
    jw_raw(&w, "dimensions", body.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&body);
}
