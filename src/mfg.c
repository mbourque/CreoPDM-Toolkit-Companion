/* mfg.c - /mfg: first-pass manufacturing inventory (session data only, no Retrieve).
 *
 * An MFG handle is NOT a ProSolid, so ProSolidFeatVisit must never be called on it
 * (that is what returned -2 before). Instead:
 *   models      : components of the top-level assembly (ProMfgAssemGet), with their MFG role
 *                 (REF_MODEL / WORKPIECE / FIXTURE) from ProAsmcompTypeGet
 *   features    : features of the MFG storage solid (ProMfgSolidGet): operations, workcells,
 *                 setups/fixtures, NC sequences
 */
#include "common.h"
#include <ProMenuBar.h>
#include <ProMfg.h>
#include <ProFeature.h>
#include <ProFeatType.h>
#include <ProModelitem.h>
#include <ProAsmcomp.h>

#define MAX_FEATS 2048
typedef struct {
    Jw *w; int count; ProAssembly top;
    int ids[MAX_FEATS]; char is_op[MAX_FEATS]; int nfeat;   /* pass-1 index for parent lookup */
    int nest_missing;
    ProSolid store;                                          /* MFG storage solid */
} MfgCtx;

static const char *role_str(ProAsmcompType t)
{
    switch (t) {
    case PRO_ASM_COMP_TYPE_WORKPIECE: return "WORKPIECE";
    case PRO_ASM_COMP_TYPE_REF_MODEL: return "REF_MODEL";
    case PRO_ASM_COMP_TYPE_FIXTURE:   return "FIXTURE";
    case PRO_ASM_COMP_TYPE_NONE:      return "NONE";
    default:                          return "OTHER";
    }
}

static const char *mfg_type_str(ProMfgType t)
{
    switch (t) {
    case PRO_MFGTYPE_MACH_PART:   return "MACH_PART";
    case PRO_MFGTYPE_MACH_ASSEM:  return "MACH_ASSEM";
    case PRO_MFGTYPE_SHEET_METAL: return "SHEET_METAL";
    case PRO_MFGTYPE_MOLD:        return "MOLD";
    case PRO_MFGTYPE_CAST:        return "CAST";
    case PRO_MFGTYPE_CMM:         return "CMM";
    default:                      return "OTHER";
    }
}

/* ---------- models (reference / workpiece / fixture) ---------- */
static void model_cb(const CompInfo *ci, void *ud)
{
    MfgCtx *mc = (MfgCtx *)ud;
    ProAsmcompType rt = PRO_ASM_COMP_TYPE_NONE;
    int have_role = (ci->feat && ProAsmcompTypeGet(ci->feat, mc->top, &rt) == PRO_TK_NO_ERROR);
    Jw *w = mc->w;
    char idS[40];

    sprintf(idS, "mfgmdl:%d", ci->id);
    jw_obj(w, NULL);
    jw_str(w, "id", idS);
    jw_int(w, "creo_id", ci->id);
    jw_str(w, "name", ci->file);
    jw_str(w, "type", ci->kind);
    jw_str(w, "role", have_role ? role_str(rt) : NULL);
    jw_int(w, "level", 1);
    jw_str(w, "status", ci->status);
    jw_bool(w, "suppressed", ci->suppressed);
    jw_end_obj(w);
    mc->count++;
}

/* ---------- features of the storage solid ---------- */
static int has_word(const char *s, const char *word) { return strstr(s, word) != NULL; }

static const char *classify_feature(ProFeattype ft, const char *type)
{
    if (ft == PRO_FEAT_OPERATION) return "OPERATION";
    if (ft == PRO_FEAT_WORKCELL)  return "WORKCELL";
    if (ft == PRO_FEAT_FIXSETUP)  return "SETUP";
    if (has_word(type, "FIXTURE")) return "FIXTURE";
    if (has_word(type, "STOCK"))   return "STOCK_MODEL";
    /* NC sequences: Toolkit has no single type for them; recognise by type-name keywords (heuristic). */
    if (has_word(type, "SEQ") || has_word(type, "MILL") || has_word(type, "TURN") ||
        has_word(type, "DRILL") || has_word(type, "HOLE") || has_word(type, "FACING") ||
        has_word(type, "ROUGH") || has_word(type, "FINISH") || has_word(type, "PROFIL") ||
        has_word(type, "TRAJ") || has_word(type, "ENGRAV") || has_word(type, "THREAD") ||
        has_word(type, "CUTLINE") || has_word(type, "MANUAL") || has_word(type, "CMM") ||
        has_word(type, "NC_"))
        return "NC_SEQUENCE";
    return "OTHER";
}

static int is_geometry_noise(const char *type)
{
    return has_word(type, "DATUM") || has_word(type, "COORD") || has_word(type, "CSYS") ||
           has_word(type, "CURVE") || has_word(type, "CROSS_SEC");
}

/* pass 1: remember every feature id and whether it is an operation */
static ProError index_feat(ProFeature *feat, ProError status, ProAppData data)
{
    MfgCtx *mc = (MfgCtx *)data;
    ProFeattype ft = 0;
    (void)status;
    if (mc->nfeat < MAX_FEATS) {
        ProFeatureTypeGet(feat, &ft);
        mc->ids[mc->nfeat] = feat->id;
        mc->is_op[mc->nfeat] = (ft == PRO_FEAT_OPERATION);
        mc->nfeat++;
    }
    return PRO_TK_NO_ERROR;
}

static int is_operation_id(const MfgCtx *mc, int id)
{
    int i;
    for (i = 0; i < mc->nfeat; i++) if (mc->ids[i] == id) return mc->is_op[i];
    return 0;
}

/* operation id among the direct Toolkit parents of a feature, or 0 */
static int direct_op_parent(const MfgCtx *mc, ProFeature *feat)
{
    int *parents = NULL, np = 0, i, op = 0;
    if (ProFeatureParentsGet(feat, &parents, &np) == PRO_TK_NO_ERROR && parents) {
        for (i = 0; i < np && !op; i++)
            if (is_operation_id(mc, parents[i])) op = parents[i];
        ProArrayFree((ProArray *)&parents);
    }
    return op;
}

/* single operation shared by all dependents (children) that have one; 0 if none or ambiguous */
#define DEP_MAX_DEPTH 4
static void collect_dep_ops(const MfgCtx *mc, ProFeature *feat, int depth, int *op, int *ambiguous)
{
    int *kids = NULL, nk = 0, i;
    if (depth >= DEP_MAX_DEPTH) return;
    if (ProFeatureChildrenGet(feat, &kids, &nk) != PRO_TK_NO_ERROR || !kids) return;
    for (i = 0; i < nk; i++) {
        ProFeature child;
        int cop;
        if (ProFeatureInit(mc->store, kids[i], &child) != PRO_TK_NO_ERROR) continue;
        cop = direct_op_parent(mc, &child);
        if (cop) {
            if (!*op) *op = cop;
            else if (*op != cop) *ambiguous = 1;
        } else {
            collect_dep_ops(mc, &child, depth + 1, op, ambiguous);   /* window -> volume -> sequence chains */
        }
    }
    ProArrayFree((ProArray *)&kids);
}

static int op_from_dependents(const MfgCtx *mc, ProFeature *feat)
{
    int op = 0, ambiguous = 0;
    collect_dep_ops(mc, feat, 0, &op, &ambiguous);
    return ambiguous ? 0 : op;
}

/* pass 2 */
static ProError feat_row(ProFeature *feat, ProError status, ProAppData data)
{
    MfgCtx *mc = (MfgCtx *)data;
    Jw *w = mc->w;
    ProName tn, nm;
    ProLine sub;
    ProFeattype ft = 0;
    ProFeatStatus fs;
    char type[96], subtype[96], name[160], idS[48], parS[48];
    const char *kind, *st = "ACTIVE", *prefix;
    int supp = 0, level = 0;          /* 0 = unknown (null) */
    const char *basis = NULL;
    (void)status;

    type[0] = subtype[0] = name[0] = parS[0] = 0;
    ProFeatureTypeGet(feat, &ft);
    if (ft == PRO_FEAT_COMPONENT) return PRO_TK_NO_ERROR;   /* components are reported in models[] */
    {
        ProBoolean vis = PRO_B_TRUE;                          /* skip internal (invisible) features */
        if (ProFeatureVisibilityGet(feat, &vis) == PRO_TK_NO_ERROR && vis == PRO_B_FALSE) return PRO_TK_NO_ERROR;
    }
    if (ProFeatureTypenameGet(feat, tn) == PRO_TK_NO_ERROR) w2u(tn, type, sizeof type);
    if (!type[0]) sprintf(type, "FEATTYPE_%d", (int)ft);
    else { char *t; for (t = type; *t; ++t) *t = (*t == ' ') ? '_' : (char)toupper((unsigned char)*t); }
    if (is_geometry_noise(type)) return PRO_TK_NO_ERROR;

    sub[0] = 0;
    if (ProFeatureSubtypeGet(feat, sub) == PRO_TK_NO_ERROR) w2u(sub, subtype, sizeof subtype);
    if (ProModelitemNameGet((ProModelitem *)feat, nm) == PRO_TK_NO_ERROR) w2u(nm, name, sizeof name);
    if (is_noname_u(name)) return PRO_TK_NO_ERROR;      /* hidden unnamed feature: never emitted */
    if (!name[0]) sprintf(name, "%s (%d)", type, feat->id);

    if (ProFeatureStatusGet(feat, &fs) == PRO_TK_NO_ERROR) {
        switch (fs) {
        case PRO_FEAT_ACTIVE:              st = "ACTIVE"; break;
        case PRO_FEAT_INACTIVE:            st = "INACTIVE"; break;
        case PRO_FEAT_UNREGENERATED:       st = "UNREGENERATED"; break;
        case PRO_FEAT_SUPPRESSED:          st = "SUPPRESSED"; supp = 1; break;
        case PRO_FEAT_FAMTAB_SUPPRESSED:   st = "SUPPRESSED_FAMILY_TABLE"; supp = 1; break;
        case PRO_FEAT_SIMP_REP_SUPPRESSED: st = "SUPPRESSED_SIMPREP"; supp = 1; break;
        case PRO_FEAT_PROG_SUPPRESSED:     st = "SUPPRESSED_PROGRAM"; supp = 1; break;
        default:                           st = "UNKNOWN"; break;
        }
    }

    kind = classify_feature(ft, type);
    prefix = strcmp(kind, "OPERATION") == 0 ? "mfgop" :
             strcmp(kind, "WORKCELL") == 0 ? "mfgwc" :
             strcmp(kind, "SETUP") == 0 || strcmp(kind, "FIXTURE") == 0 ? "mfgfix" :
             strcmp(kind, "STOCK_MODEL") == 0 ? "mfgstock" :
             strcmp(kind, "NC_SEQUENCE") == 0 ? "mfgseq" : "mfgfeat";
    sprintf(idS, "%s:%d", prefix, feat->id);

    /* Nesting: operations, workcells, setups/fixtures and stock are top level. A sequence is nested under the
       operation among its Toolkit parent features; if none is found the level stays null (reported as missing). */
    if (strcmp(kind, "OPERATION") == 0 || strcmp(kind, "WORKCELL") == 0 || strcmp(kind, "SETUP") == 0 ||
        strcmp(kind, "FIXTURE") == 0 || strcmp(kind, "STOCK_MODEL") == 0) {
        level = 1;
    } else if (strcmp(kind, "NC_SEQUENCE") == 0 || strcmp(kind, "OTHER") == 0) {
        /* Sequences and every other MFG feature (windows, volumes, surfaces, drill groups, ...) */
        int op = direct_op_parent(mc, feat);
        if (op) {
            basis = "operation_reference";
        } else {
            /* Window / geometry-reference features carry no operation link of their own. Use the
               Toolkit dependency links: the sequences that reference this feature. Nest it only if
               every sequence that does has the SAME operation; shared or unreferenced -> unowned. */
            op = op_from_dependents(mc, feat);
            if (op) basis = "referenced_by_sequence";
        }
        if (op) {
            sprintf(parS, "mfgop:%d", op);
            level = 2;
        } else {
            mc->nest_missing++;
        }
    }

    jw_obj(w, NULL);
    jw_str(w, "id", idS);
    jw_int(w, "creo_id", feat->id);
    jw_str(w, "name", name);
    jw_str(w, "type", kind);
    jw_str(w, "subtype", type);              /* Toolkit feature type name */
    jw_str(w, "detail", subtype[0] ? subtype : NULL);
    if (level) jw_int(w, "level", level); else jw_null(w, "level");
    jw_str(w, "parent_id", parS[0] ? parS : NULL);
    jw_str(w, "parent_basis", basis);               /* operation_reference | referenced_by_sequence | null */
    jw_str(w, "status", st);
    jw_bool(w, "suppressed", supp);
    jw_end_obj(w);
    mc->count++;
    return PRO_TK_NO_ERROR;
}

void collect_mfg(Sb *out, const Ctx *c, const Query *q)
{
    Sb models = { 0 }, feats = { 0 };
    Jw mw, fw, w;
    MfgCtx mc, fc;
    ProMfg mfg;
    ProMfgType mt = PRO_MFGTYPE_NONE;
    ProAssembly top = NULL;
    ProSolid store = NULL;
    ProError rc;
    int skipped = 0, miss_models = 0, miss_ops = 0;
    (void)q;

    if (c->type != PRO_MDL_MFG) {
        emit_errorf(out, "mfg applies to MFG; active model is %s", c->typeStr);
        return;
    }
    mfg = (ProMfg)c->mdl;
    ProMfgTypeGet(mfg, &mt);

    /* reference / workpiece / fixture models */
    jw_init(&mw, &models);
    jw_arr(&mw, NULL);
    memset(&mc, 0, sizeof mc);
    mc.w = &mw;
    rc = ProMfgAssemGet(mfg, &top);
    if (rc == PRO_TK_NO_ERROR && top) {
        mc.top = top;
        if (walk_components((ProSolid)top, 0, model_cb, &mc, &skipped) != PRO_TK_NO_ERROR) miss_models = 1;
    } else {
        miss_models = 1;
    }
    jw_end_arr(&mw);

    /* operations / NC sequences live in the storage solid */
    jw_init(&fw, &feats);
    jw_arr(&fw, NULL);
    memset(&fc, 0, sizeof fc);
    fc.w = &fw;
    rc = ProMfgSolidGet(mfg, &store);
    if (rc == PRO_TK_NO_ERROR && store) {
        fc.store = store;
        ProSolidFeatVisit(store, index_feat, NULL, (ProAppData)&fc);          /* pass 1: ids + operations */
        rc = ProSolidFeatVisit(store, feat_row, NULL, (ProAppData)&fc);       /* pass 2: rows */
        if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) miss_ops = 1;
    } else {
        miss_ops = 1;
    }
    jw_end_arr(&fw);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_str(&w, "mfg_type", mfg_type_str(mt));
    jw_bool(&w, "partial", miss_models || miss_ops || fc.nest_missing);
    jw_arr(&w, "partial_missing");
    if (miss_models) jw_str(&w, NULL, "models");
    if (miss_ops)    jw_str(&w, NULL, "operations");
    if (fc.nest_missing) jw_str(&w, NULL, "nesting");   /* some sequences have no resolvable operation parent */
    jw_end_arr(&w);
    jw_int(&w, "modelCount", mc.count);
    jw_int(&w, "featureCount", fc.count);
    jw_int(&w, "skippedNoModel", skipped);
    jw_raw(&w, "models", models.p);
    jw_raw(&w, "features", feats.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&models);
    sb_free(&feats);
}
