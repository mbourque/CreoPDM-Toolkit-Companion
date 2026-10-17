/* sketch.c - /sketch: contents of a standalone 2D section (.sec).
 *
 * A retrieved .sec model handle IS the section handle (ProMdlnameRetrieve(..., PRO_MDLFILE_2DSECTION, (ProMdl *)&section) in the
 * Toolkit pt_section example), so the ProSection* / ProSecdim* readers are used on it directly. The section is only READ:
 * no ProSectionRegenerate / Solve (they would modify the model), so the stored geometry is reported as saved.
 * Fields Toolkit cannot supply are null, never guessed (fully_dimensioned, over_constrained, strong/weak, regenerated,
 * dimension reference flag and tolerance). Angles in entity items are DEGREES (Toolkit stores radians); lengths are in the
 * section's own length unit. Constraints that are denied (disabled) are not listed. */
#include "common.h"
#include <ProMenuBar.h>
#include <ProSection.h>
#include <ProSecdim.h>
#include <ProSecConstr.h>
#include <Pro2dEntdef.h>
#include <ProArray.h>
#include <ProMdlUnits.h>
#include <math.h>

#define ITEMS_MAX 2000
#define PI_ 3.14159265358979323846

typedef struct { double x0, y0, x1, y1; int has; } Box;
typedef struct { double x, y; int deg; int parent; } Vtx;

static void box_add(Box *b, double x, double y)
{
    if (x != x || y != y) return;
    if (!b->has) { b->x0 = b->x1 = x; b->y0 = b->y1 = y; b->has = 1; return; }
    if (x < b->x0) b->x0 = x;
    if (x > b->x1) b->x1 = x;
    if (y < b->y0) b->y0 = y;
    if (y > b->y1) b->y1 = y;
}

static void arc_ends(const Pro2dArcdef *a, double *sx, double *sy, double *ex, double *ey)
{
    *sx = a->center[0] + a->radius * cos(a->start_angle);
    *sy = a->center[1] + a->radius * sin(a->start_angle);
    *ex = a->center[0] + a->radius * cos(a->end_angle);
    *ey = a->center[1] + a->radius * sin(a->end_angle);
}

static void box_arc(Box *b, const Pro2dArcdef *a)
{
    double sx, sy, ex, ey, a0 = a->start_angle, a1 = a->end_angle;
    int k;
    arc_ends(a, &sx, &sy, &ex, &ey);
    box_add(b, sx, sy);
    box_add(b, ex, ey);
    if (a1 <= a0) a1 += 2.0 * PI_;
    for (k = 0; k < 4; k++) {                       /* axis extremes that lie inside the counter-clockwise sweep */
        double ang = k * PI_ / 2.0;
        while (ang < a0) ang += 2.0 * PI_;
        if (ang <= a1) box_add(b, a->center[0] + a->radius * cos(ang), a->center[1] + a->radius * sin(ang));
    }
}

static const char *ent_label(Pro2dEntType t)
{
    switch (t) {
    case PRO_2D_LINE:        return "line";
    case PRO_2D_ARC:         return "arc";
    case PRO_2D_CIRCLE:      return "circle";
    case PRO_2D_SPLINE:
    case PRO_2D_B_SPLINE:    return "spline";
    case PRO_2D_POINT:       return "point";
    case PRO_2D_CENTER_LINE: return "centerline";
    case PRO_2D_TEXT:        return "text";
    default:                 return "other";
    }
}

static void put_pt(Jw *w, const char *key, double x, double y)
{
    jw_arr(w, key);
    jw_dbl(w, NULL, x);
    jw_dbl(w, NULL, y);
    jw_end_arr(w);
}

static const char *constr_label(ProConstraintType t)
{
    switch (t) {
    case PRO_CONSTRAINT_HORIZONTAL_ENT: return "horizontal";
    case PRO_CONSTRAINT_VERTICAL_ENT:   return "vertical";
    case PRO_CONSTRAINT_TANGENT_ENTS:   return "tangent";
    case PRO_CONSTRAINT_SAME_POINT:     return "coincident";
    case PRO_CONSTRAINT_EQUAL_RADII:
    case PRO_CONSTRAINT_EQUAL_SEGMENTS: return "equal";
    case PRO_CONSTRAINT_PARALLEL_ENTS:  return "parallel";
    case PRO_CONSTRAINT_ORTHOG_ENTS:    return "perpendicular";
    case PRO_CONSTRAINT_SYMMETRY:       return "symmetric";
    case PRO_CONSTRAINT_MIDDLE_POINT:   return "midpoint";
    case PRO_CONSTRAINT_PNT_ON_ENT:     return "point_on";
    default:                            return "other";
    }
}

static const char *dim_label(ProSecdimType t)
{
    switch (t) {
    case PRO_TK_DIM_RAD:             return "radial";
    case PRO_TK_DIM_DIA:
    case PRO_TK_DIM_DIA_LINEAR:      return "diameter";
    case PRO_TK_DIM_ARC_ANGLE:
    case PRO_TK_DIM_LINES_ANGLE:
    case PRO_TK_DIM_LINE_CURVE_ANGLE:
    case PRO_TK_DIM_3_PNT_ANGLE:
    case PRO_TK_DIM_TOT_INC_ANG:
    case PRO_TK_DIM_ANG_POLAR:       return "angular";
    case PRO_TK_DIM_LINE:
    case PRO_TK_DIM_LINE_POINT:
    case PRO_TK_DIM_LINE_LINE:
    case PRO_TK_DIM_PNT_PNT:
    case PRO_TK_DIM_PNT_PNT_HORIZ:
    case PRO_TK_DIM_PNT_PNT_VERT:
    case PRO_TK_DIM_AOC_AOC_TAN_HORIZ:
    case PRO_TK_DIM_AOC_AOC_TAN_VERT:
    case PRO_TK_DIM_LINE_AOC:
    case PRO_TK_DIM_LIN_MULTI_OFFSET:
    case PRO_TK_DIM_PNT_OFFSET:
    case PRO_TK_DIM_PNT_PNT_ORI:
    case PRO_TK_DIM_AOC_AOC_ORI:     return "linear";
    default:                         return "other";
    }
}

/* ---- loops: endpoints of the non-construction curves are clustered into vertices; a connected group is a closed loop
   when every vertex in it is shared by exactly two curve ends. Full circles / ellipses are closed loops by themselves. ---- */
static int vfind(Vtx *v, int i) { while (v[i].parent != i) { v[i].parent = v[v[i].parent].parent; i = v[i].parent; } return i; }

static int vertex_of(Vtx *v, int *nv, double x, double y, double eps)
{
    int i;
    for (i = 0; i < *nv; i++)
        if (fabs(v[i].x - x) <= eps && fabs(v[i].y - y) <= eps) return i;
    v[*nv].x = x; v[*nv].y = y; v[*nv].deg = 0; v[*nv].parent = *nv;
    return (*nv)++;
}

/* ---- entity geometry rebuilt from ProSectionEntityEval samples (used when ProSectionEntityGet rejects the section) ---- */
typedef struct {
    int ok;                                   /* geometry could be classified */
    union { Pro2dEntdef base; Pro2dLinedef line; Pro2dArcdef arc; Pro2dCircledef circle; Pro2dSplinedef spline; Pro2dPointdef point; } u;
    Pro2dPnt pts[9];
} SynEnt;

static double norm_ang(double a)
{
    while (a < 0) a += 2.0 * PI_;
    while (a >= 2.0 * PI_) a -= 2.0 * PI_;
    return a;
}

/* Samples the entity at t = 0, 1/8 ... 1 and classifies it: point, line (collinear), circle / arc (concyclic), else spline. */
static int synth_entity(ProSection sec, int id, SynEnt *s)
{
    double P[9][2], span = 0.0, L, ax, ay, bx, by, cx, cy, d, ux, uy, r;
    int k;
    memset(s, 0, sizeof *s);
    for (k = 0; k < 9; k++) {
        ProVector pt, d1, d2;
        if (ProSectionEntityEval(sec, id, k / 8.0, pt, d1, d2) != PRO_TK_NO_ERROR) return 0;
        P[k][0] = pt[0]; P[k][1] = pt[1];
    }
    for (k = 1; k < 9; k++) { double dd = hypot(P[k][0] - P[0][0], P[k][1] - P[0][1]); if (dd > span) span = dd; }
    L = hypot(P[8][0] - P[0][0], P[8][1] - P[0][1]);
    if (span < 1e-9) {                                             /* all samples coincide: a point */
        s->u.point.type = PRO_2D_POINT; s->u.point.pnt[0] = P[0][0]; s->u.point.pnt[1] = P[0][1]; s->ok = 1; return 1;
    }
    if (L > 1e-6 * span) {                                         /* open: line? */
        int line = 1;
        for (k = 1; k < 8; k++) {
            double cr = (P[8][0] - P[0][0]) * (P[k][1] - P[0][1]) - (P[8][1] - P[0][1]) * (P[k][0] - P[0][0]);
            if (fabs(cr) / L > 1e-6 * L) { line = 0; break; }
        }
        if (line) {
            s->u.line.type = PRO_2D_LINE;
            s->u.line.end1[0] = P[0][0]; s->u.line.end1[1] = P[0][1];
            s->u.line.end2[0] = P[8][0]; s->u.line.end2[1] = P[8][1];
            s->ok = 1; return 1;
        }
    }
    ax = P[1][0]; ay = P[1][1]; bx = P[4][0]; by = P[4][1]; cx = P[7][0]; cy = P[7][1];
    d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (fabs(d) > 1e-12) {
        int circ = 1;
        ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d;
        uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d;
        r = hypot(ax - ux, ay - uy);
        for (k = 0; k < 9; k++) if (fabs(hypot(P[k][0] - ux, P[k][1] - uy) - r) > 1e-5 * r) { circ = 0; break; }
        if (circ) {
            if (L <= 1e-6 * span) {                                /* closed: full circle */
                s->u.circle.type = PRO_2D_CIRCLE; s->u.circle.center[0] = ux; s->u.circle.center[1] = uy; s->u.circle.radius = r;
            } else {                                               /* arc, stored counter-clockwise start -> end */
                double a0 = atan2(P[0][1] - uy, P[0][0] - ux), a8 = atan2(P[8][1] - uy, P[8][0] - ux);
                double cr = (P[0][0] - ux) * (P[4][1] - uy) - (P[0][1] - uy) * (P[4][0] - ux);
                s->u.arc.type = PRO_2D_ARC; s->u.arc.center[0] = ux; s->u.arc.center[1] = uy; s->u.arc.radius = r;
                if (cr >= 0) { s->u.arc.start_angle = norm_ang(a0); s->u.arc.end_angle = norm_ang(a8); }
                else         { s->u.arc.start_angle = norm_ang(a8); s->u.arc.end_angle = norm_ang(a0); }
            }
            s->ok = 1; return 1;
        }
    }
    for (k = 0; k < 9; k++) { s->pts[k][0] = P[k][0]; s->pts[k][1] = P[k][1]; }
    s->u.spline.type = PRO_2D_SPLINE; s->u.spline.n_points = 9; s->u.spline.point_arr = s->pts;
    s->ok = 1;
    return 1;
}

static ProError ent_get(ProSection sec, int id, SynEnt *syn, int idx, Pro2dEntdef **pe)
{
    if (syn) {
        if (!syn[idx].ok) return PRO_TK_INVALID_TYPE;
        *pe = &syn[idx].u.base;
        return PRO_TK_NO_ERROR;
    }
    return ProSectionEntityGet(sec, id, pe);
}

/* Largest entity id referenced by any dimension or constraint (0 when none could be read). */
static int max_ref_id(ProSection sec)
{
    ProIntlist dids = NULL, cids = NULL;
    int dn = 0, cn = 0, i, j, mx = 0;
    if (ProSecdimIdsGet(sec, &dids, &dn) == PRO_TK_NO_ERROR) {
        for (i = 0; i < dn; i++) {
            int *rid = NULL, nr = 0;
            ProSectionPointType *pt = NULL;
            if (ProSecdimReferencesGet(sec, dids[i], &rid, &pt, &nr) == PRO_TK_NO_ERROR) {
                for (j = 0; j < nr && rid; j++) if (rid[j] > mx) mx = rid[j];
                if (rid) ProArrayFree((ProArray *)&rid);
                if (pt) ProArrayFree((ProArray *)&pt);
            }
        }
        if (dids) ProArrayFree((ProArray *)&dids);
    }
    if (ProSectionConstraintsIdsGet(sec, &cids, &cn) == PRO_TK_NO_ERROR) {
        for (i = 0; i < cn; i++) {
            ProConstraintType ct;
            ProConstraintStatus cs;
            int nr = 0, *ents = NULL;
            ProSectionPointType *senses = NULL;
            if (ProSectionConstraintsGet(sec, cids[i], &ct, &cs, &nr, &ents, &senses) == PRO_TK_NO_ERROR) {
                for (j = 0; j < nr && ents; j++) if (ents[j] > mx) mx = ents[j];
                if (ents) ProArrayFree((ProArray *)&ents);
                if (senses) ProArrayFree((ProArray *)&senses);
            }
        }
        if (cids) ProArrayFree((ProArray *)&cids);
    }
    return mx;
}

void collect_sketch(Sb *out, const Ctx *c, const Query *q)
{
    Sb body = { 0 }, items = { 0 };
    Jw w, iw;
    ProSection sec = (ProSection)c->mdl;
    ProIntlist ids = NULL;
    int n = 0, i, k;
    int counts[8] = { 0 }, total = 0, items_n = 0, truncated = 0, construction_n = 0;
    int cnt_closed = 0, cnt_open = 0, loops_ok = 1;
    Box box;
    Vtx *vt = NULL;
    int nv = 0, *sega = NULL, *segb = NULL, nseg = 0;
    double eps = 1e-6;
    ProUnitsystem us;
    ProUnititem it;
    char lunit[40];
    ProError rc;
    char miss[12][480], note[4][480];
    int nnote = 0;
    int nmiss = 0, ent_ok = 1, nfail = 0, ids_heap = 0;
    SynEnt *syn = NULL;
#define MISS(...) do { if (nmiss < 12) { _snprintf(miss[nmiss], sizeof miss[0], __VA_ARGS__); miss[nmiss][sizeof miss[0] - 1] = 0; nmiss++; } } while (0)
#define NOTE(...) do { if (nnote < 4) { _snprintf(note[nnote], sizeof note[0], __VA_ARGS__); note[nnote][sizeof note[0] - 1] = 0; nnote++; } } while (0)
    static const char *lab[8] = { "line", "arc", "circle", "spline", "point", "centerline", "text", "other" };
    (void)q;

    if (c->type != PRO_MDL_2DSECTION) {
        emit_errorf(out, "sketch applies to SECTION (.sec); active model is %s", c->typeStr);
        return;
    }
    memset(&box, 0, sizeof box);
    lunit[0] = 0;
    if (ProMdlPrincipalunitsystemGet(c->mdl, &us) == PRO_TK_NO_ERROR &&
        ProUnitsystemUnitGet(&us, PRO_UNITTYPE_LENGTH, &it) == PRO_TK_NO_ERROR)
        w2u(it.name, lunit, sizeof lunit);

    rc = ProSectionEntityIdsGet(sec, &ids, &n);
    if (rc != PRO_TK_NO_ERROR) {
        /* ProSectionEntityIdsGet / ProSectionEntityGet reject this section with PRO_TK_BAD_INPUTS, while
           ProSectionEntityEval / IsConstruction work on the same ids. So: ids are found by probing ProSectionEntityEval
           over 1..N (N from the entity ids dimensions and constraints reference), and each entity's geometry is
           rebuilt from 9 evaluated points. No regenerate / solve / Intent Manager change is made to the section. */
        ProIntlist ids2 = NULL;
        int n2 = 0;
        ProError r2;
        ids = NULL; n = 0;
        r2 = ProSectionSubsectionEntityIdsGet(sec, &ids2, &n2);
        if (r2 == PRO_TK_NO_ERROR && n2 > 0) {
            ids = ids2; n = n2;
            NOTE("entities: ProSectionEntityIdsGet=%d; ids from ProSectionSubsectionEntityIdsGet", (int)rc);
        } else {
            int mx = max_ref_id(sec), id, found = 0, cap = 64, run = 0, capped = 0, last = 0;
            const int HARD = 50000, GAP = 400;          /* stop after GAP consecutive misses past the highest referenced id */
            if (ids2) ProArrayFree((ProArray *)&ids2);
            ids = (int *)malloc(sizeof(int) * (size_t)cap);
            syn = (SynEnt *)calloc((size_t)cap, sizeof(SynEnt));
            ids_heap = 1;
            for (id = 1; ids && syn; id++) {
                ProVector pt, d1, d2;
                if (id > HARD) { capped = 1; break; }
                if (ProSectionEntityEval(sec, id, 0.5, pt, d1, d2) == PRO_TK_NO_ERROR) {
                    if (found == cap) {
                        int *ni; SynEnt *ns;
                        cap *= 2;
                        ni = (int *)realloc(ids, sizeof(int) * (size_t)cap);
                        ns = (SynEnt *)realloc(syn, sizeof(SynEnt) * (size_t)cap);
                        if (ni) ids = ni;
                        if (ns) syn = ns;
                        if (!ni || !ns) { capped = 1; break; }
                        memset(syn + found, 0, sizeof(SynEnt) * (size_t)(cap - found));
                    }
                    ids[found] = id;
                    synth_entity(sec, id, &syn[found]);
                    found++;
                    last = id;
                    run = 0;
                } else if (id >= mx && ++run >= GAP) break;
            }
            n = found;
            if (capped)
                MISS("entities: probed ids 1..%d only (stopped at the safety cap); %d found, counts may be short", id - 1, found);
            if (found > 0)
                NOTE("entities: ProSectionEntityIdsGet=%d, ProSectionSubsectionEntityIdsGet=%d, ProSectionEntityGet rejects the section; ids found by probing ProSectionEntityEval over 1..%d (%d found, highest id %d) and geometry rebuilt from Eval samples (type inferred as line/arc/circle/spline; centerline, point and text cannot be told apart)",
                     (int)rc, (int)r2, id - 1, found, last);
            else {
                ent_ok = 0; nfail++;
                MISS("entities: ProSectionEntityIdsGet=%d, ProSectionSubsectionEntityIdsGet=%d; probing ProSectionEntityEval over 1..%d found none",
                     (int)rc, (int)r2, id - 1);
            }
        }
    }
    if (n > 0) {
        vt = (Vtx *)malloc(sizeof(Vtx) * (size_t)n * 2);
        sega = (int *)malloc(sizeof(int) * (size_t)n);
        segb = (int *)malloc(sizeof(int) * (size_t)n);
        if (!vt || !sega || !segb) loops_ok = 0;
    }

    /* pass 1: outline (needed to scale the vertex tolerance) */
    for (i = 0; i < n; i++) {
        Pro2dEntdef *e = NULL;
        ProBoolean cons = PRO_B_FALSE;
        if (ent_get(sec, ids[i], syn, i, &e) != PRO_TK_NO_ERROR || !e) continue;
        ProSectionEntityIsConstruction(sec, ids[i], &cons);
        if (cons == PRO_B_TRUE || e->type == PRO_2D_CENTER_LINE) continue;
        switch (e->type) {
        case PRO_2D_LINE:   { Pro2dLinedef *l = (Pro2dLinedef *)e; box_add(&box, l->end1[0], l->end1[1]); box_add(&box, l->end2[0], l->end2[1]); } break;
        case PRO_2D_ARC:    box_arc(&box, (Pro2dArcdef *)e); break;
        case PRO_2D_CIRCLE: { Pro2dCircledef *ci = (Pro2dCircledef *)e;
                              box_add(&box, ci->center[0] - ci->radius, ci->center[1] - ci->radius);
                              box_add(&box, ci->center[0] + ci->radius, ci->center[1] + ci->radius); } break;
        case PRO_2D_POINT:  { Pro2dPointdef *p = (Pro2dPointdef *)e; box_add(&box, p->pnt[0], p->pnt[1]); } break;
        case PRO_2D_SPLINE: { Pro2dSplinedef *s = (Pro2dSplinedef *)e; unsigned j;
                              for (j = 0; j < s->n_points && s->point_arr; j++) box_add(&box, s->point_arr[j][0], s->point_arr[j][1]); } break;
        case PRO_2D_B_SPLINE: { Pro2dBSplinedef *s = (Pro2dBSplinedef *)e; int j;
                              for (j = 0; j < s->num_c_points && s->c_pnts; j++) box_add(&box, s->c_pnts[j][0], s->c_pnts[j][1]); } break;
        case PRO_2D_ELLIPSE: { Pro2dEllipsedef *el = (Pro2dEllipsedef *)e;
                              box_add(&box, el->origin[0] - el->x_radius, el->origin[1] - el->y_radius);
                              box_add(&box, el->origin[0] + el->x_radius, el->origin[1] + el->y_radius); } break;
        default: break;
        }
    }
    if (box.has) {
        double sz = fmax(box.x1 - box.x0, box.y1 - box.y0);
        eps = (sz > 0 ? sz : 1.0) * 1e-6;
    }

    jw_init(&iw, &items);
    jw_arr(&iw, NULL);

    /* pass 2: items, counts, loop graph */
    for (i = 0; i < n; i++) {
        Pro2dEntdef *e = NULL;
        ProBoolean cons = PRO_B_FALSE;
        const char *label;
        int idx;
        double px0 = 0, py0 = 0, px1 = 0, py1 = 0;
        int has_ends = 0;
        ProError er = ent_get(sec, ids[i], syn, i, &e);

        total++;
        if (er != PRO_TK_NO_ERROR || !e) {                 /* type Toolkit cannot represent: still counted */
            counts[7]++;
            if (nmiss < 6) MISS("entity %d: ProSectionEntityGet=%d", ids[i], (int)er);
            if (items_n < ITEMS_MAX) { jw_obj(&iw, NULL); jw_int(&iw, "id", ids[i]); jw_str(&iw, "type", "other"); jw_null(&iw, "construction"); jw_end_obj(&iw); items_n++; }
            else truncated = 1;
            continue;
        }
        ProSectionEntityIsConstruction(sec, ids[i], &cons);
        label = ent_label(e->type);
        for (idx = 0; idx < 8; idx++) if (strcmp(lab[idx], label) == 0) break;
        counts[idx]++;
        if (cons == PRO_B_TRUE) construction_n++;

        if (items_n < ITEMS_MAX) {
            jw_obj(&iw, NULL);
            jw_int(&iw, "id", ids[i]);
            jw_str(&iw, "type", label);
            switch (e->type) {
            case PRO_2D_LINE: case PRO_2D_CENTER_LINE: {
                Pro2dLinedef *l = (Pro2dLinedef *)e;
                put_pt(&iw, "start", l->end1[0], l->end1[1]);
                put_pt(&iw, "end", l->end2[0], l->end2[1]);
            } break;
            case PRO_2D_ARC: {
                Pro2dArcdef *a = (Pro2dArcdef *)e;
                put_pt(&iw, "center", a->center[0], a->center[1]);
                jw_dbl(&iw, "radius", a->radius);
                jw_dbl(&iw, "start_angle", a->start_angle * 180.0 / PI_);
                jw_dbl(&iw, "end_angle", a->end_angle * 180.0 / PI_);
            } break;
            case PRO_2D_CIRCLE: {
                Pro2dCircledef *ci = (Pro2dCircledef *)e;
                put_pt(&iw, "center", ci->center[0], ci->center[1]);
                jw_dbl(&iw, "radius", ci->radius);
            } break;
            case PRO_2D_POINT: {
                Pro2dPointdef *p = (Pro2dPointdef *)e;
                put_pt(&iw, "position", p->pnt[0], p->pnt[1]);
            } break;
            case PRO_2D_SPLINE: {
                Pro2dSplinedef *s = (Pro2dSplinedef *)e;
                jw_int(&iw, "point_count", s->n_points);
                if (s->n_points && s->point_arr) {
                    put_pt(&iw, "start", s->point_arr[0][0], s->point_arr[0][1]);
                    put_pt(&iw, "end", s->point_arr[s->n_points - 1][0], s->point_arr[s->n_points - 1][1]);
                }
            } break;
            case PRO_2D_B_SPLINE: {
                Pro2dBSplinedef *s = (Pro2dBSplinedef *)e;
                jw_int(&iw, "point_count", s->num_c_points);
                if (s->num_c_points > 0 && s->c_pnts) {
                    put_pt(&iw, "start", s->c_pnts[0][0], s->c_pnts[0][1]);
                    put_pt(&iw, "end", s->c_pnts[s->num_c_points - 1][0], s->c_pnts[s->num_c_points - 1][1]);
                }
            } break;
            case PRO_2D_TEXT: {
                Pro2dTextdef *t = (Pro2dTextdef *)e;
                jw_wstr(&iw, "text", t->string);
                put_pt(&iw, "first_corner", t->first_corner[0], t->first_corner[1]);
                put_pt(&iw, "second_corner", t->second_corner[0], t->second_corner[1]);
            } break;
            default: break;
            }
            jw_bool(&iw, "construction", cons == PRO_B_TRUE);
            jw_end_obj(&iw);
            items_n++;
        } else {
            truncated = 1;
        }

        /* loop graph input (real geometry only) */
        if (cons == PRO_B_TRUE || e->type == PRO_2D_CENTER_LINE || !loops_ok) continue;
        switch (e->type) {
        case PRO_2D_LINE:  { Pro2dLinedef *l = (Pro2dLinedef *)e; px0 = l->end1[0]; py0 = l->end1[1]; px1 = l->end2[0]; py1 = l->end2[1]; has_ends = 1; } break;
        case PRO_2D_ARC:   arc_ends((Pro2dArcdef *)e, &px0, &py0, &px1, &py1); has_ends = 1; break;
        case PRO_2D_SPLINE: { Pro2dSplinedef *s = (Pro2dSplinedef *)e;
                              if (s->n_points >= 2 && s->point_arr) { px0 = s->point_arr[0][0]; py0 = s->point_arr[0][1];
                                  px1 = s->point_arr[s->n_points - 1][0]; py1 = s->point_arr[s->n_points - 1][1]; has_ends = 1; } } break;
        case PRO_2D_B_SPLINE: { Pro2dBSplinedef *s = (Pro2dBSplinedef *)e;
                              if (s->num_c_points >= 2 && s->c_pnts) { px0 = s->c_pnts[0][0]; py0 = s->c_pnts[0][1];
                                  px1 = s->c_pnts[s->num_c_points - 1][0]; py1 = s->c_pnts[s->num_c_points - 1][1]; has_ends = 1; } } break;
        case PRO_2D_CIRCLE:
        case PRO_2D_ELLIPSE: cnt_closed++; break;
        default: break;
        }
        if (has_ends) {
            int a = vertex_of(vt, &nv, px0, py0, eps), b = vertex_of(vt, &nv, px1, py1, eps);
            vt[a].deg++; vt[b].deg++;
            sega[nseg] = a; segb[nseg] = b; nseg++;
        }
    }
    jw_end_arr(&iw);

    if (loops_ok && nseg > 0) {
        int *bad = (int *)calloc((size_t)nv, sizeof(int)), *used = (int *)calloc((size_t)nv, sizeof(int));
        if (bad && used) {
            for (k = 0; k < nseg; k++) {                   /* union the two ends of every curve */
                int ra = vfind(vt, sega[k]), rb = vfind(vt, segb[k]);
                if (ra != rb) vt[ra].parent = rb;
            }
            for (k = 0; k < nv; k++) {
                int r = vfind(vt, k);
                used[r] = 1;
                if (vt[k].deg != 2) bad[r] = 1;
            }
            for (k = 0; k < nv; k++) if (used[k]) { if (bad[k]) cnt_open++; else cnt_closed++; }
        } else loops_ok = 0;
        free(bad); free(used);
    }
    free(vt); free(sega); free(segb);
    if (ids) { if (ids_heap) free(ids); else ProArrayFree((ProArray *)&ids); }
    free(syn);

    jw_init(&w, &body);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);

    jw_obj(&w, "units");
    jw_str(&w, "length", lunit[0] ? lunit : NULL);
    jw_str(&w, "angle", "deg");
    jw_end_obj(&w);

    if (box.has) {
        jw_obj(&w, "outline");
        put_pt(&w, "min", box.x0, box.y0);
        put_pt(&w, "max", box.x1, box.y1);
        jw_dbl(&w, "width", box.x1 - box.x0);
        jw_dbl(&w, "height", box.y1 - box.y0);
        jw_end_obj(&w);
    } else {
        jw_null(&w, "outline");
    }

    if (ent_ok) {
        jw_obj(&w, "entities");
        jw_int(&w, "total", total);
        jw_obj(&w, "counts");
        for (k = 0; k < 8; k++) jw_int(&w, lab[k], counts[k]);
        jw_end_obj(&w);
        jw_int(&w, "construction_count", construction_n);
        jw_raw(&w, "items", items.p ? items.p : "[]");
        jw_bool(&w, "truncated", truncated);
        jw_end_obj(&w);
    } else {
        jw_null(&w, "entities");
    }

    /* dimensions: always complete; null when they could not be read */
    {
        ProIntlist dids = NULL;
        int dn = 0;
        ProError drc = ProSecdimIdsGet(sec, &dids, &dn);
        if (drc != PRO_TK_NO_ERROR) { nfail++; jw_null(&w, "dimensions"); MISS("dimensions: ProSecdimIdsGet=%d", (int)drc); }
        else {
        jw_arr(&w, "dimensions");
        {
            for (i = 0; i < dn; i++) {
                ProSecdimType dt = PRO_TK_DIM_TYPE_UNKNOWN;
                double val = 0.0;
                char idb[24], disp[40];
                int *rid = NULL, nr = 0;
                ProSectionPointType *pt = NULL;
                ProError vrc = ProSecdimValueGet(sec, dids[i], &val), trc = ProSecdimTypeGet(sec, dids[i], &dt);
                int has_val = (vrc == PRO_TK_NO_ERROR);
                if (!has_val) MISS("dimension sd%d: ProSecdimValueGet=%d", dids[i], (int)vrc);
                if (trc != PRO_TK_NO_ERROR) MISS("dimension sd%d: ProSecdimTypeGet=%d", dids[i], (int)trc);
                sprintf(idb, "sd%d", dids[i]);
                jw_obj(&w, NULL);
                jw_str(&w, "id", idb);
                jw_str(&w, "type", dim_label(dt));
                if (has_val) { sprintf(disp, "%.3f", val); jw_dbl(&w, "value", val); jw_str(&w, "display", disp); }
                else         { jw_null(&w, "value"); jw_null(&w, "display"); }
                jw_arr(&w, "entity_ids");
                if (ProSecdimReferencesGet(sec, dids[i], &rid, &pt, &nr) == PRO_TK_NO_ERROR) {
                    int j;
                    for (j = 0; j < nr; j++) jw_int(&w, NULL, rid[j]);
                    if (rid) ProArrayFree((ProArray *)&rid);
                    if (pt) ProArrayFree((ProArray *)&pt);
                }
                jw_end_arr(&w);
                jw_null(&w, "reference");           /* Toolkit does not expose the reference flag */
                jw_null(&w, "tolerance");           /* ... nor a section dimension tolerance */
                jw_end_obj(&w);
            }
            if (dids) ProArrayFree((ProArray *)&dids);
        }
        jw_end_arr(&w);
        }
    }

    /* constraints: always complete (enabled ones); null when they could not be read */
    {
        ProIntlist cids = NULL;
        int cn = 0;
        ProError crc = ProSectionConstraintsIdsGet(sec, &cids, &cn);
        if (crc != PRO_TK_NO_ERROR) { nfail++; jw_null(&w, "constraints"); MISS("constraints: ProSectionConstraintsIdsGet=%d", (int)crc); }
        else {
        jw_arr(&w, "constraints");
        {
            for (i = 0; i < cn; i++) {
                ProConstraintType ct = PRO_CONSTRAINT_TYPE_UNKNOWN;
                ProConstraintStatus cs = PRO_TK_CONSTRAINT_ENABLED;
                int nr = 0, *ents = NULL, j;
                ProSectionPointType *senses = NULL;
                ProError grc = ProSectionConstraintsGet(sec, cids[i], &ct, &cs, &nr, &ents, &senses);
                if (grc != PRO_TK_NO_ERROR) { MISS("constraint %d: ProSectionConstraintsGet=%d", cids[i], (int)grc); continue; }
                if (cs == PRO_TK_CONSTRAINT_ENABLED) {
                    jw_obj(&w, NULL);
                    jw_str(&w, "type", constr_label(ct));
                    jw_arr(&w, "entity_ids");
                    for (j = 0; j < nr && ents; j++) jw_int(&w, NULL, ents[j]);
                    jw_end_arr(&w);
                    jw_end_obj(&w);
                }
                if (ents) ProArrayFree((ProArray *)&ents);
                if (senses) ProArrayFree((ProArray *)&senses);
            }
            if (cids) ProArrayFree((ProArray *)&cids);
        }
        jw_end_arr(&w);
        }
    }

    if (loops_ok && ent_ok) {
        jw_obj(&w, "loops");
        jw_int(&w, "closed", cnt_closed);
        jw_int(&w, "open", cnt_open);
        jw_end_obj(&w);
    } else {
        jw_null(&w, "loops");
    }

    jw_obj(&w, "status");
    jw_null(&w, "fully_dimensioned");
    jw_null(&w, "over_constrained");
    jw_null(&w, "strong_dimensions");
    jw_null(&w, "weak_dimensions");
    jw_null(&w, "regenerated");                    /* never regenerated, so not known */
    jw_end_obj(&w);

    if (nmiss) {
        jw_arr(&w, "partial_missing");
        for (k = 0; k < nmiss; k++) jw_str(&w, NULL, miss[k]);
        jw_end_arr(&w);
    }
    if (nnote) {                                        /* informational: how the data was obtained, not a gap in it */
        jw_arr(&w, "notes");
        for (k = 0; k < nnote; k++) jw_str(&w, NULL, note[k]);
        jw_end_arr(&w);
    }

    jw_end_obj(&w);
    if (nfail >= 3) {                                   /* nothing at all could be read: a real failure, not a partial result */
        char msg[1200];
        int len = _snprintf(msg, sizeof msg, "could not read the section:");
        for (k = 0; k < nmiss && len > 0 && len < (int)sizeof msg - 260; k++) len += _snprintf(msg + len, sizeof msg - len, " %s;", miss[k]);
        msg[sizeof msg - 1] = 0;
        emit_error(out, msg);
        sb_free(&body);
        sb_free(&items);
        return;
    }
    sb_puts(out, body.p ? body.p : "null");
    sb_puts(out, "\n");
    sb_free(&body);
    sb_free(&items);
}

/* ---- /sketch_probe: diagnostic. Return codes of the section readers on this handle, so a failing entity read can be
   understood. Reads only, except ?solve=1 which also tries ProSectionSolve (modifies the session section). ---- */
void collect_sketch_probe(Sb *out, const Ctx *c, const Query *q)
{
    Jw w;
    ProSection sec = (ProSection)c->mdl;
    ProVector pt, d1, d2;
    ProBoolean b = PRO_B_FALSE, im = PRO_B_FALSE;
    ProSectionShape shape;
    ProMatrix m;
    double eps = 0, val = 0;
    int idx[4] = { 2, 3, 4, 5 }, k, nid = 0, *ids = NULL;
    ProName nm;
    ProSection act = NULL;
    Pro2dEntdef *e = NULL;
    Pro2dPnt loc;
    char v[8];
    if (c->type != PRO_MDL_2DSECTION) { emit_errorf(out, "sketch_probe applies to SECTION (.sec)"); return; }
    jw_init(&w, out);
    jw_obj(&w, NULL);
    jw_bool(&w, "ok", 1);
    jw_int(&w, "ProSectionNameGet", (int)ProSectionNameGet(sec, nm));
    jw_int(&w, "ProSectionLocationGet", (int)ProSectionLocationGet(sec, m));
    jw_int(&w, "ProSectionEpsilonGet", (int)ProSectionEpsilonGet(sec, &eps));
    jw_int(&w, "ProSectionShapeGet", (int)ProSectionShapeGet(sec, &shape));
    jw_int(&w, "ProSectionIntentManagerModeGet", (int)ProSectionIntentManagerModeGet(sec, &im));
    jw_bool(&w, "intent_manager_mode", im == PRO_B_TRUE);
    jw_int(&w, "ProSectionActiveGet", (int)ProSectionActiveGet(&act));
    jw_bool(&w, "sketcher_active_copy", act != NULL);
    jw_int(&w, "ProSecdimLocationGet_sd0", (int)ProSecdimLocationGet(sec, 0, loc));
    jw_int(&w, "ProSecdimValueGet_sd0", (int)ProSecdimValueGet(sec, 0, &val));
    jw_arr(&w, "per_entity");
    {   /* ?ids=1180,1183,... (default 2,3,4,5) */
        int list[64], nl = 0;
        char idq[400];
        if (q_get_dec(q, "ids", idq, sizeof idq) && idq[0]) {
            const char *p = idq;
            while (*p && nl < 64) { list[nl++] = atoi(p); while (*p && *p != ',') p++; if (*p == ',') p++; }
        } else { for (k = 0; k < 4; k++) list[nl++] = idx[k]; }
        for (k = 0; k < nl; k++) {
            double t, ts[3] = { 0.0, 0.5, 1.0 };
            int j;
            ProBoolean bc = PRO_B_FALSE, bp = PRO_B_FALSE;
            ProError rcons, rproj;
            Pro2dPnt xy = { 0.0, 0.0 };
            jw_obj(&w, NULL);
            jw_int(&w, "id", list[k]);
            {   /* SAFETY: only ids that ProSectionEntityEval accepts get the other calls; section getters on a
                   non-existent id crashed Creo. */
                ProVector tp, td1, td2;
                ProError ex = ProSectionEntityEval(sec, list[k], 0.5, tp, td1, td2);
                if (ex != PRO_TK_NO_ERROR) {
                    jw_int(&w, "Eval_0.5_rc", (int)ex);
                    jw_str(&w, "note", "Eval rejects this id: no other call made (not a curve, or no such entity)");
                    jw_end_obj(&w);
                    continue;
                }
            }
            rcons = ProSectionEntityIsConstruction(sec, list[k], &bc);
            rproj = ProSectionEntityIsProjection(sec, list[k], &bp);
            jw_int(&w, "IsConstruction_rc", (int)rcons); jw_bool(&w, "IsConstruction", bc == PRO_B_TRUE);
            jw_int(&w, "IsProjection_rc", (int)rproj);   jw_bool(&w, "IsProjection", bp == PRO_B_TRUE);
            for (j = 0; j < 3; j++) {
                char key[24];
                ProError re = ProSectionEntityEval(sec, list[k], ts[j], pt, d1, d2);
                _snprintf(key, sizeof key, "Eval_%g_rc", ts[j]); key[sizeof key - 1] = 0;
                jw_int(&w, key, (int)re);
                if (re == PRO_TK_NO_ERROR) { _snprintf(key, sizeof key, "p_%g", ts[j]); key[sizeof key - 1] = 0; put_pt(&w, key, pt[0], pt[1]); }
                if (j == 0 && re == PRO_TK_NO_ERROR) { jw_dbl(&w, "d1_len", sqrt(d1[0] * d1[0] + d1[1] * d1[1])); jw_dbl(&w, "d2_len", sqrt(d2[0] * d2[0] + d2[1] * d2[1])); }
            }
            jw_int(&w, "ParamEval_rc", (int)ProSectionEntityParamEval(sec, list[k], xy, &t));
            jw_end_obj(&w);
        }
    }
    jw_end_arr(&w);
    if (q_get(q, "solve", v, sizeof v) && v[0] == '1') {
        ProWSecerror se = NULL;
        ProSecerrorAlloc(&se);
        jw_int(&w, "ProSectionSolve", (int)ProSectionSolve(sec, &se));
        if (se) ProSecerrorFree(&se);
        jw_int(&w, "ProSectionEntityIdsGet_after_solve", (int)ProSectionEntityIdsGet(sec, &ids, &nid));
        jw_int(&w, "entity_count_after_solve", nid);
        if (ids) ProArrayFree((ProArray *)&ids);
    }
    jw_end_obj(&w);
    sb_puts(out, "\n");
}
