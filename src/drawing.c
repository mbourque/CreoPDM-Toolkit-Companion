/* drawing.c - /drawing: sheets, views, notes, dimensions, tables.
 *
 * Rows share one shape (id as unique string, creo_id numeric, name, type, subtype, level,
 * status, suppressed, sheet, sheet_id, parent_id) plus type-specific extras.
 * Drawing-only APIs; models already in session are read, nothing is retrieved. */
#include "common.h"
#include <ProDrawing.h>
#include <ProDimension.h>
#include <ProDtlnote.h>
#include <ProTable.h>
#include <ProModelitem.h>
#include <ProMdlUnits.h>
#include <ProArray.h>

#define MAX_SOLIDS 256

typedef struct {
    Jw        *w;
    ProDrawing drw;
    int        nsheets;
    int        cursheet;
    int        dims;
    ProMdl     solids[MAX_SOLIDS];
    int        nsolids;
} DCtx;

static void add_solid(DCtx *d, ProMdl m)
{
    int i;
    if (!m) return;
    for (i = 0; i < d->nsolids; i++) if (d->solids[i] == m) return;
    if (d->nsolids < MAX_SOLIDS) d->solids[d->nsolids++] = m;
}

static void sheet_ref(Jw *w, int sheet)
{
    if (sheet > 0) {
        char s[24];
        sprintf(s, "sheet:%d", sheet);
        jw_str(w, "sheet_id", s);
    } else {
        jw_null(w, "sheet_id");
    }
    if (sheet > 0) jw_int(w, "sheet", sheet); else jw_null(w, "sheet");
}

static void unit_of_sheet(ProDrawing drw, int sheet, char *buf, int n)
{
    ProUnititem u;
    buf[0] = 0;
    if (sheet > 0 && ProDrawingSheetUnitsGet(drw, sheet, &u) == PRO_TK_NO_ERROR) w2u(u.name, buf, n);
}
static void length_unit_of_mdl(ProMdl m, char *buf, int n)
{
    ProUnitsystem us;
    ProUnititem it;
    buf[0] = 0;
    if (ProMdlPrincipalunitsystemGet(m, &us) == PRO_TK_NO_ERROR &&
        ProUnitsystemUnitGet(&us, PRO_UNITTYPE_LENGTH, &it) == PRO_TK_NO_ERROR)
        w2u(it.name, buf, n);
}

/* Model whose length unit applies to a drawing-owned dimension: the model it is attached to,
   else the model shown in its view, else the first model shown in the drawing. No retrieve. */
static ProMdl referenced_model(DCtx *d, ProDimension *dim, int has_view, ProView view)
{
    ProSelection *att = NULL;
    ProDimSense *sense = NULL;
    ProMdl m = NULL;

    if (ProDrawingDimAttachsGet(d->drw, dim, &att, &sense) == PRO_TK_NO_ERROR && att) {
        int n = 0, i;
        ProArraySizeGet((ProArray)att, &n);
        for (i = 0; i < n && !m; i++) {
            ProModelitem mi;
            if (att[i] && ProSelectionModelitemGet(att[i], &mi) == PRO_TK_NO_ERROR &&
                mi.owner && (ProMdl)mi.owner != (ProMdl)d->drw)
                m = (ProMdl)mi.owner;
        }
        ProSelectionarrayFree(att);
        if (sense) ProArrayFree((ProArray *)&sense);
    }
    if (!m && has_view) {
        ProSolid vs = NULL;
        if (ProDrawingViewSolidGet(d->drw, view, &vs) == PRO_TK_NO_ERROR && vs) m = (ProMdl)vs;
    }
    if (!m && d->nsolids > 0) m = d->solids[0];
    return m;
}

/* ---------- notes ---------- */
static int note_text(ProDtlnotedata nd, char *out, int outsz)
{
    ProDtlnoteline *lines = NULL;
    int nl = 0, i, j, len = 0;
    out[0] = 0;
    if (ProDtlnotedataLinesCollect(nd, &lines) != PRO_TK_NO_ERROR || !lines) return 0;
    ProArraySizeGet((ProArray)lines, &nl);
    for (i = 0; i < nl; i++) {
        ProDtlnotetext *texts = NULL;
        int nt = 0;
        if (i > 0 && len < outsz - 1) out[len++] = '\n';
        if (ProDtlnotelineTextsCollect(lines[i], &texts) != PRO_TK_NO_ERROR || !texts) continue;
        ProArraySizeGet((ProArray)texts, &nt);
        for (j = 0; j < nt; j++) {
            ProLine s;
            char u[400];
            s[0] = 0;
            if (ProDtlnotetextStringGet(texts[j], s) == PRO_TK_NO_ERROR) {
                int ul;
                w2u(s, u, sizeof u);
                ul = (int)strlen(u);
                if (len + ul < outsz - 1) { memcpy(out + len, u, ul); len += ul; }
            }
        }
        ProArrayFree((ProArray *)&texts);
    }
    out[len] = 0;
    ProArrayFree((ProArray *)&lines);
    return 1;
}

/* returns 0 ok / not found, else the failing ProError */
static int collect_notes(DCtx *d, int *count)
{
    int s, bad = 0;
    for (s = 1; s <= d->nsheets; s++) {
        ProDtlnote *notes = NULL;
        int n = 0, i;
        ProError rc = ProDrawingDtlnotesCollect(d->drw, NULL, s, &notes);
        if (rc == PRO_TK_E_NOT_FOUND) continue;
        if (rc != PRO_TK_NO_ERROR || !notes) { bad = (int)rc; continue; }
        ProArraySizeGet((ProArray)notes, &n);
        for (i = 0; i < n; i++) {
            ProDtlnotedata nd = NULL;
            char text[4096], idS[40], nm[96], par[24];
            ProName mn;
            ProBoolean shown = PRO_B_TRUE;
            int nid = notes[i].id;

            text[0] = 0;
            if (ProDtlnoteDataGet(&notes[i], NULL, PRODISPMODE_NUMERIC, &nd) == PRO_TK_NO_ERROR && nd) {
                note_text(nd, text, sizeof text);
                ProDtlnotedataIsDisplayed(nd, &shown);
                ProDtlnotedataFree(nd);
            } else {
                bad = 1;
            }
            sprintf(idS, "note:%d", nid);
            sprintf(par, "sheet:%d", s);
            nm[0] = 0;
            if (ProModelitemNameGet((ProModelitem *)&notes[i], mn) == PRO_TK_NO_ERROR) w2u(mn, nm, sizeof nm);
            if (!nm[0]) sprintf(nm, "Note %d", nid);

            jw_obj(d->w, NULL);
            jw_str(d->w, "id", idS);
            jw_int(d->w, "creo_id", nid);
            jw_str(d->w, "name", nm);
            jw_str(d->w, "type", "NOTE");
            jw_str(d->w, "subtype", "");
            jw_int(d->w, "level", 2);
            jw_str(d->w, "status", shown == PRO_B_TRUE ? "DISPLAYED" : "ERASED");
            jw_bool(d->w, "suppressed", 0);
            sheet_ref(d->w, s);
            jw_str(d->w, "parent_id", par);
            jw_str(d->w, "text", text);
            jw_end_obj(d->w);
            (*count)++;
        }
        ProArrayFree((ProArray *)&notes);
    }
    return bad;
}

/* ---------- dimensions ---------- */
typedef struct { DCtx *d; int check_displayed; int reference; int *count; } DimVisit;

static const char *dim_type_str(ProDimensiontype t)
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

static ProError dim_visit(ProDimension *dim, ProError status, ProAppData data)
{
    DimVisit *dv = (DimVisit *)data;
    DCtx *d = dv->d;
    ProBoolean disp = PRO_B_TRUE;
    ProView view;
    ProDimensiontype dt = PRODIMTYPE_UNKNOWN;
    ProError trc;
    const char *subtype, *basis;
    int type_known, units_assumed = 0;
    ProMdlName on;
    ProName nm, sym;
    double val = 0.0;
    int has_val, vid = -1, sheet = 0, has_view = 0;
    char idS[128], owner[200], nameU[96], symU[96], unit[32], par[24], vS[24], units_model[200];
    double shown = 0.0;
    int has_shown;
    (void)status;

    if (dv->check_displayed &&
        (ProDrawingDimensionIsDisplayed(d->drw, dim, &disp) != PRO_TK_NO_ERROR || disp != PRO_B_TRUE))
        return PRO_TK_NO_ERROR;                 /* only dimensions actually shown in this drawing */

    owner[0] = 0;
    if (ProMdlMdlnameGet((ProMdl)dim->owner, on) == PRO_TK_NO_ERROR) {
        ProMdlType ot;
        w2u(on, owner, sizeof owner);
        if (ProMdlTypeGet((ProMdl)dim->owner, &ot) == PRO_TK_NO_ERROR)
            strcat(owner, ot == PRO_MDL_DRAWING ? ".drw" : ot == PRO_MDL_ASSEMBLY ? ".asm" : ".prt");
        lower_ascii(owner);
    }
    nameU[0] = symU[0] = 0;
    if (ProModelitemNameGet((ProModelitem *)dim, nm) == PRO_TK_NO_ERROR) w2u(nm, nameU, sizeof nameU);
    if (ProDimensionSymbolGet(dim, sym) == PRO_TK_NO_ERROR) w2u(sym, symU, sizeof symU);
    trc = ProDimensionTypeGet(dim, &dt);
    type_known = (trc == PRO_TK_NO_ERROR) &&
                 (dt == PRODIMTYPE_LINEAR || dt == PRODIMTYPE_RADIUS || dt == PRODIMTYPE_DIAMETER ||
                  dt == PRODIMTYPE_ANGLE || dt == PRODIMTYPE_ARC_LENGTH);
    if (type_known) {
        subtype = dim_type_str(dt);
    } else {
        /* No ordinate probe: ProDimensionIsOrdinate pulls in a license option we do not have. */
        subtype = (trc == PRO_TK_NO_ERROR) ? "OTHER" : "UNKNOWN";   /* raw id in type_id */
    }
    has_val =(ProDimensionValueGet(dim, &val) == PRO_TK_NO_ERROR);
    has_shown = (ProDimensionDisplayedValueGet(dim, &shown) == PRO_TK_NO_ERROR);

    if (ProDrawingDimensionViewGet(d->drw, dim, &view) == PRO_TK_NO_ERROR &&
        ProDrawingViewIdGet(d->drw, view, &vid) == PRO_TK_NO_ERROR) {
        has_view = 1;
        ProDrawingViewSheetGet(d->drw, view, &sheet);
    }

    /* Units policy (values are never converted; the unit names the value as Creo stores it):
         ANGLE                     -> "deg"
         everything else (length)  -> MODEL length unit, never the drawing sheet unit:
           solid-owned dimension   -> its owner model
           drawing-owned dimension -> the model it measures (attachment, else view model, else first view model)
       Toolkit has no per-dimension unit call, so the unit comes from that model. */
    unit[0] = 0;
    units_model[0] = 0;
    if (type_known && dt == PRODIMTYPE_ANGLE) {
        strcpy(unit, "deg");
        basis = "angle";
    } else {
        ProMdl um = (ProMdl)dim->owner;
        if (um == (ProMdl)d->drw) um = referenced_model(d, dim, has_view, view);
        if (um && ProMdlMdlnameGet(um, on) == PRO_TK_NO_ERROR) {
            ProMdlType ut;
            w2u(on, units_model, sizeof units_model);
            if (ProMdlTypeGet(um, &ut) == PRO_TK_NO_ERROR)
                strcat(units_model, ut == PRO_MDL_ASSEMBLY ? ".asm" : ".prt");
            lower_ascii(units_model);
            length_unit_of_mdl(um, unit, sizeof unit);
        }
        basis = unit[0] ? "owner_model" : "unresolved";
    }
    /* If the dimension type could not be confirmed, a length unit is an assumption: say so. */
    if (!type_known) units_assumed = 1;

    sprintf(idS, "dim:%s:%d", owner[0] ? owner : "?", dim->id);
    if (has_view) { sprintf(par, "view:%d", vid); sprintf(vS, "view:%d", vid); }
    else if (sheet > 0) sprintf(par, "sheet:%d", sheet);
    else par[0] = 0;

    jw_obj(d->w, NULL);
    jw_str(d->w, "id", idS);
    jw_int(d->w, "creo_id", dim->id);
    jw_str(d->w, "name", nameU[0] ? nameU : symU);
    jw_str(d->w, "type", "DIMENSION");
    jw_str(d->w, "subtype", subtype);
    if (trc == PRO_TK_NO_ERROR) jw_int(d->w, "type_id", (int)dt); else jw_null(d->w, "type_id");
    jw_int(d->w, "level", has_view ? 3 : 2);
    jw_str(d->w, "status", "DISPLAYED");
    jw_bool(d->w, "suppressed", 0);
    sheet_ref(d->w, sheet);
    jw_str(d->w, "parent_id", par[0] ? par : NULL);
    jw_str(d->w, "view_id", has_view ? vS : NULL);
    if (has_val) jw_dbl(d->w, "value", val); else jw_null(d->w, "value");
    jw_str(d->w, "units", unit[0] ? unit : NULL);   /* derived, see policy above */
    jw_str(d->w, "units_basis", basis);             /* angle | owner_model | unresolved (never the sheet unit) */
    jw_str(d->w, "units_model", units_model[0] ? units_model : NULL);   /* model whose unit was used */
    if (has_shown) jw_dbl(d->w, "value_displayed", shown); else jw_null(d->w, "value_displayed");
    if (units_assumed) jw_bool(d->w, "units_assumed", 1);
    jw_str(d->w, "symbol", symU[0] ? symU : NULL);
    jw_str(d->w, "owner", owner[0] ? owner : NULL);
    jw_bool(d->w, "reference", dv->reference);
    jw_end_obj(d->w);
    (*dv->count)++;
    return PRO_TK_NO_ERROR;
}

typedef struct { DCtx *d; } SolidCollect;
static void solid_cb(const CompInfo *ci, void *ud)
{
    SolidCollect *sc = (SolidCollect *)ud;
    if (ci->mdl) add_solid(sc->d, ci->mdl);
}

static int collect_dims(DCtx *d, int *count)
{
    ProView *views = NULL;
    int nv = 0, i, bad = 0;
    DimVisit dv;
    ProError rc;

    dv.d = d; dv.count = count;

    /* dimensions created in the drawing itself */
    dv.check_displayed = 0; dv.reference = 0;
    rc = ProDrawingDimensionVisit(d->drw, PRO_DIMENSION, dim_visit, NULL, (ProAppData)&dv);
    if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) bad = (int)rc;
    dv.reference = 1;
    rc = ProDrawingDimensionVisit(d->drw, PRO_REF_DIMENSION, dim_visit, NULL, (ProAppData)&dv);
    if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) bad = (int)rc;

    /* dimensions shown from the models in the views (models already in session) */
    if (ProDrawingViewsCollect(d->drw, &views) == PRO_TK_NO_ERROR && views) {
        ProArraySizeGet((ProArray)views, &nv);
        for (i = 0; i < nv; i++) {
            ProSolid vs = NULL;
            if (ProDrawingViewSolidGet(d->drw, views[i], &vs) == PRO_TK_NO_ERROR && vs) {
                ProMdlType t;
                add_solid(d, (ProMdl)vs);
                if (ProMdlTypeGet((ProMdl)vs, &t) == PRO_TK_NO_ERROR && t == PRO_MDL_ASSEMBLY) {
                    SolidCollect sc;
                    int sk = 0;
                    sc.d = d;
                    walk_components(vs, 1, solid_cb, &sc, &sk);
                }
            }
        }
        ProArrayFree((ProArray *)&views);
    }
    dv.check_displayed = 1;
    for (i = 0; i < d->nsolids; i++) {
        dv.reference = 0;
        rc = ProSolidDimensionVisit((ProSolid)d->solids[i], PRO_B_FALSE, dim_visit, NULL, (ProAppData)&dv);
        if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) bad = (int)rc;
        dv.reference = 1;
        rc = ProSolidDimensionVisit((ProSolid)d->solids[i], PRO_B_TRUE, dim_visit, NULL, (ProAppData)&dv);
        if (rc != PRO_TK_NO_ERROR && rc != PRO_TK_E_NOT_FOUND) bad = (int)rc;
    }
    return bad;
}

/* ---------- tables ---------- */
static int collect_tables(DCtx *d, int *count, int with_cells)
{
    ProTable *tables = NULL;
    int n = 0, i, bad = 0;
    ProError rc = ProTablesCollect((ProMdl)d->drw, &tables);
    if (rc == PRO_TK_E_NOT_FOUND) return 0;
    if (rc != PRO_TK_NO_ERROR || !tables) return (int)rc;
    ProArraySizeGet((ProArray)tables, &n);
    for (i = 0; i < n; i++) {
        ProTable *t = &tables[i];
        int rows = 0, cols = 0, sheet = 0, segs = 0;
        char idS[40], nm[96], par[24];
        ProName mn;

        ProTableRowsColumnsCount(t, &rows, &cols);
        ProTableSegCount(t, &segs);
        if (ProTableSegSheetGet(t, 0, &sheet) != PRO_TK_NO_ERROR) ProTableSegSheetGet(t, 1, &sheet);
        nm[0] = 0;
        if (ProModelitemNameGet((ProModelitem *)t, mn) == PRO_TK_NO_ERROR) w2u(mn, nm, sizeof nm);
        if (!nm[0]) sprintf(nm, "Table %d", t->id);
        sprintf(idS, "table:%d", t->id);
        if (sheet > 0) sprintf(par, "sheet:%d", sheet); else par[0] = 0;

        jw_obj(d->w, NULL);
        jw_str(d->w, "id", idS);
        jw_int(d->w, "creo_id", t->id);
        jw_str(d->w, "name", nm);
        jw_str(d->w, "type", "TABLE");
        jw_str(d->w, "subtype", "");
        jw_int(d->w, "level", 2);
        jw_str(d->w, "status", "DISPLAYED");
        jw_bool(d->w, "suppressed", 0);
        sheet_ref(d->w, sheet);
        jw_str(d->w, "parent_id", par[0] ? par : NULL);
        jw_int(d->w, "rows", rows);
        jw_int(d->w, "columns", cols);
        jw_int(d->w, "segments", segs);
        if (with_cells) {                         /* optional cell dump (?cells=1), capped */
            int r, c, rcap = rows > 200 ? 200 : rows, ccap = cols > 30 ? 30 : cols;
            jw_arr(d->w, "cells");
            for (r = 1; r <= rcap; r++) {
                jw_arr(d->w, NULL);
                for (c = 1; c <= ccap; c++) {
                    ProWstring *lines = NULL;
                    char cell[512];
                    int nl = 0, k, len = 0;
                    cell[0] = 0;
                    if (ProTableCelltextGet(t, c, r, PROTABLE_NORMAL, &lines) == PRO_TK_NO_ERROR && lines) {
                        ProArraySizeGet((ProArray)lines, &nl);
                        for (k = 0; k < nl; k++) {
                            char u[200];
                            int ul;
                            w2u(lines[k], u, sizeof u);
                            ul = (int)strlen(u);
                            if (k && len < (int)sizeof cell - 1) cell[len++] = '\n';
                            if (len + ul < (int)sizeof cell - 1) { memcpy(cell + len, u, ul); len += ul; }
                        }
                        cell[len] = 0;
                        ProWstringproarrayFree(lines);
                    }
                    jw_str(d->w, NULL, cell);
                }
                jw_end_arr(d->w);
            }
            jw_end_arr(d->w);
        }
        jw_end_obj(d->w);
        (*count)++;
    }
    ProArrayFree((ProArray *)&tables);
    return bad;
}

/* ---------- /drawing ---------- */
void collect_drawing(Sb *out, const Ctx *c, const Query *q)
{
    Jw w, fw;
    Sb feats = { 0 };
    DCtx d;
    ProDrawing drw;
    int nsheets = 0, cur = 0, i, nviews = 0;
    int ncount = 0, dcount = 0, tcount = 0;
    int bad_notes, bad_dims, bad_tables;
    ProView *views = NULL;
    char v[8];
    int with_cells = (q_get(q, "cells", v, sizeof v) && v[0] == '1');

    if (c->type != PRO_MDL_DRAWING) {
        emit_errorf(out, "drawing applies to DRAWING; active model is %s", c->typeStr);
        return;
    }
    drw = (ProDrawing)c->mdl;
    if (ProDrawingSheetsCount(drw, &nsheets) != PRO_TK_NO_ERROR) {
        emit_error(out, "ProDrawingSheetsCount failed");
        return;
    }
    ProDrawingCurrentSheetGet(drw, &cur);

    memset(&d, 0, sizeof d);
    d.drw = drw; d.nsheets = nsheets; d.cursheet = cur;
    jw_init(&fw, &feats);
    jw_arr(&fw, NULL);
    d.w = &fw;

    /* sheets */
    for (i = 1; i <= nsheets; i++) {
        ProName sn;
        char sid[24];
        jw_obj(&fw, NULL);
        sprintf(sid, "sheet:%d", i);
        jw_str(&fw, "id", sid);
        jw_int(&fw, "creo_id", i);
        sn[0] = 0;
        if (ProDrawingSheetNameGet(drw, i, sn) == PRO_TK_NO_ERROR && sn[0]) jw_wstr(&fw, "name", sn);
        else { char b[24]; sprintf(b, "Sheet %d", i); jw_str(&fw, "name", b); }
        jw_str(&fw, "type", "SHEET");
        jw_str(&fw, "subtype", "");
        jw_int(&fw, "level", 1);
        jw_str(&fw, "status", i == cur ? "CURRENT" : "ACTIVE");
        jw_bool(&fw, "suppressed", 0);
        jw_end_obj(&fw);
    }

    /* views */
    if (ProDrawingViewsCollect(drw, &views) == PRO_TK_NO_ERROR && views) {
        ProArraySizeGet((ProArray)views, &nviews);
        for (i = 0; i < nviews; i++) {
            ProName vn;
            int vid = -1, sheet = 0;
            ProSolid vs = NULL;
            ProMdlName mn;
            char sub[300], vidS[24];
            sub[0] = 0; vn[0] = 0;
            ProDrawingViewIdGet(drw, views[i], &vid);
            ProDrawingViewNameGet(drw, views[i], vn);
            ProDrawingViewSheetGet(drw, views[i], &sheet);
            if (ProDrawingViewSolidGet(drw, views[i], &vs) == PRO_TK_NO_ERROR && vs &&
                ProMdlMdlnameGet((ProMdl)vs, mn) == PRO_TK_NO_ERROR) {
                char b[200];
                w2u(mn, b, sizeof b);
                _snprintf(sub, sizeof sub, "%s", b);
                sub[sizeof sub - 1] = 0;
                lower_ascii(sub);
            }
            jw_obj(&fw, NULL);
            sprintf(vidS, "view:%d", vid);
            jw_str(&fw, "id", vidS);
            jw_int(&fw, "creo_id", vid);
            jw_wstr(&fw, "name", vn);
            jw_str(&fw, "type", "VIEW");
            jw_str(&fw, "subtype", sub);          /* model displayed (name without extension) */
            jw_int(&fw, "level", 2);
            jw_str(&fw, "status", "ACTIVE");
            jw_bool(&fw, "suppressed", 0);
            sheet_ref(&fw, sheet);
            if (sheet > 0) { char p[24]; sprintf(p, "sheet:%d", sheet); jw_str(&fw, "parent_id", p); }
            else jw_null(&fw, "parent_id");
            jw_end_obj(&fw);
        }
        ProArrayFree((ProArray *)&views);
    }

    bad_notes  = collect_notes(&d, &ncount);
    bad_dims   = collect_dims(&d, &dcount);
    bad_tables = collect_tables(&d, &tcount, with_cells);
    jw_end_arr(&fw);

    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_str(&w, "fileName", c->fileName);
    jw_bool(&w, "partial", bad_notes || bad_dims || bad_tables);
    jw_arr(&w, "partial_missing");           /* empty when everything was readable */
    if (bad_notes)  jw_str(&w, NULL, "notes");
    if (bad_dims)   jw_str(&w, NULL, "dimensions");
    if (bad_tables) jw_str(&w, NULL, "tables");
    jw_end_arr(&w);
    jw_int(&w, "sheetCount", nsheets);
    jw_int(&w, "viewCount", nviews);
    jw_int(&w, "noteCount", ncount);
    jw_int(&w, "dimensionCount", dcount);
    jw_int(&w, "tableCount", tcount);
    jw_raw(&w, "features", feats.p);
    jw_end_obj(&w);
    sb_puts(out, "\n");
    sb_free(&feats);
}
