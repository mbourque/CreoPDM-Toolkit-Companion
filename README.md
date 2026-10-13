# CreoPDM Toolkit service (Pro/TOOLKIT aux app)

One loaded Toolkit app that serves CreoPDM Collect metadata over a localhost HTTP API, one endpoint per domain.
Grew out of the "Structure Export" pilot; the DLL is still `creopdm_structure.dll` so existing registration works.
Not wired into CreoPDM / Creo.JS yet.

> **Status (v0.3.0):** builds and unlocks cleanly against Creo 13.4.1.0 headers. `/structure` was verified in Creo
> earlier (pilot, 14 components). **Everything else, and the new HTTP routing, is untested in Creo.** Treat the first
> run as a test pass (matrix below).

## Layout
| File | Role |
|---|---|
| `src/main.c` | `user_initialize`/`user_terminate`, ribbon command (`creopdm_structure_export` -> `/structure` to a file) |
| `src/http_server.c` | 127.0.0.1 server, worker thread, main-thread marshalling |
| `src/router.c` | path -> collector table, `/health`, `/collect` |
| `src/common.c/.h` | string builder, JSON writer, logging, model context |
| `src/structure.c` | component walker + `/structure` |
| `src/model.c` | identity, units, parameters, mass, materials |
| `src/features.c` | features (non-component), relations |
| `src/assembly.c` | constraints, simplified reps |
| `src/drawing.c` | drawing sheets + views |

Add a domain: write `collect_x(Sb*, const Ctx*, const Query*)`, add one row to `DOMAINS[]` in `router.c`. It then gets
its own `GET /x` and is available in `/collect?domains=x`.

## HTTP
Binds **127.0.0.1 only**. Port: env `CREOPDM_TOOLKIT_PORT`, else (old name) `CREOPDM_STRUCTURE_PORT`, default **8767**.
Per-request timeout: `CREOPDM_TOOLKIT_TIMEOUT_MS` (default 30000). Set env vars before starting Creo.
Errors are HTTP 200 + `{"ok": false, "error": "..."}` (404 unknown path, 405 bad method). `GET` and `POST` are equivalent.

| Endpoint | PART | ASM | MFG | DRW | Response |
|---|:-:|:-:|:-:|:-:|---|
| `/health` | x | x | x | x | `{ok, service:"creopdm-toolkit", version, model:{fileName,type}\|null}` (`busy:true` if Creo is busy) |
| `/identity` | x | x | x | x | `{ok,fileName,identity:{file_name,common_name,full_name,generic_name,instance_name,origin,model_type,model_role,is_modified,version_stamp}}` |
| `/units` | x | x | x | x | `{ok,fileName,units:{system_name,length,mass,time,temperature,angle}}` |
| `/parameters` | x | x | x | x | `{ok,fileName,parameters:[{name,value,data_type,units,description,is_designated}]}` |
| `/mass` | x | x | | | `{ok,fileName,mass:{mass,volume,surface_area,density,gravity_center[3],principal_moments[3]|null,cg_inertia_tensor[3][3]},method}`; assemblies try 3 Toolkit calls, then a component sum (see below) |
| `/materials` | x | | | | `{ok,fileName,materials:{current,names[]}}` |
| `/features` | x | x | | x | `{ok,fileName,features:[{id,name,type,subtype,level,status,suppressed}]}`; components excluded; for DRW same rows as `/drawing` |
| `/relations` | x | x | | | `{ok,fileName,normal[],post_regeneration[],features:[{feature_id,feature_name,relations[]}],errors[]}` |
| `/structure` | | x | | | `{ok,fileName,componentCount,skippedNoModel,recurse,components:[...]}`; `?recurse=0\|1` |
| `/constraints` | | x | | | `{ok,fileName,recurse,components:[{id,name,path,level,suppressed,constraints[]}],errors[]}`; `?recurse=0\|1` |
| `/simp-reps` | | x | | | `{ok,fileName,active:{id,name,is_master},representations:[{id,name,is_active}],errors[]}` |
| `/drawing` | | | | x | `{ok,fileName,partial,partial_missing[],sheetCount,viewCount,noteCount,dimensionCount,tableCount,features:[SHEET/VIEW/NOTE/DIMENSION/TABLE rows]}`; string ids `sheet:N` `view:N` `note:N` `dim:<owner>:N` `table:N`, numeric id in `creo_id`; `?cells=1` adds table cell text |
| `/collect?domains=a,b` | x | x | x | x | `{ok,fileName,type,domains:{a:{...},b:{...}}}` |

`/collect` with no `domains` (or `all`) runs every domain that applies to the active model type and omits the rest.
With an explicit list, a domain that does not apply returns its own `{ok:false,error}` while the others still run.
`simp-reps` and `simp_reps` are interchangeable.

### Mapping to CreoMetadataRequest (pdm-lite)
Take the inner key: `/parameters`.parameters -> `parameters`, `/mass`.mass -> `mass`, `/units`.units -> `units`,
`/materials`.materials -> `materials`, `/identity`.identity -> `identity`, `/features`.features -> `features`,
`/structure`.components -> `structure`. `relations`, `constraints`, `simp_reps` are the response itself (the
envelope dicts). Parameter `value` is always a string (`"true"/"false"` for BOOLEAN); `units` only for DOUBLE.

### Honest limits
- **Mass** returns `ok:false` (plus `volume` if known) when no density/material exists; it never returns zeros.
  It is computed on the active model only and loads nothing.
- **`/structure?recurse=1`** walks sub-assemblies that are **already in session**; others report `expanded:false`.
  Nothing is retrieved. Same for `/constraints?recurse=1`.
- **`/constraints`** field names follow the Creo.JS row keys (`type, offset, offset_units, assembly_reference,
  component_reference, *_datum_side`), but the reference object (`item_type,id,name,path`) and the component wrapper are
  Toolkit-native and **provisional**. `offset_units` is null (not derived). Datum sides are raw enum ints.
- **`/identity`**: `generic_name`, `instance_name`, `model_role` are empty strings (not derived); `version_stamp` null.
- **`/features`** `level` is null (flat list); `type` is the Toolkit feature type name, which may differ from Creo.JS labels.
- **`/simp-reps`**: id/name/is_active only. Non-positive Toolkit ids (e.g. DEFAULT REP) are reported as `null`; the active rep is matched by name.
- **`/drawing`**: notes, dimensions and tables are read; `partial` turns true only if one of those enumerations errors (then named in `partial_missing`). Dimension `units` are derived (owner model length unit, sheet unit for drawing-created dims, `deg` for angles) because Toolkit exposes no per-dimension unit. Shown dims come from models already in session (view models plus in-session components of assembly views). Notes inside table cells and model annotations (3D notes, GD&T) are not listed.
- Dimensions, family table, BOM are not provided.

## Threading (what was chosen)
Toolkit is not thread-safe, so the HTTP worker thread never calls it. `user_initialize` runs on Creo's main thread and
creates a hidden message-only window; each request is `PostMessage`d to it and executed in its window procedure via
Creo's own message pump. Requests are handled one at a time (sequential accept loop), so exports never overlap. If
Creo is busy (modal dialog, regeneration) the call times out with `"Creo busy ... timed out"`; `/health` answers
within 3 s regardless. If the port is in use, the server is skipped, the reason is logged and Creo's message line
says so; the ribbon command still works.

## Build / unlock / reload
```bat
build.bat
unlock.bat
```
Close Creo first (it locks the DLL); rebuilding un-unlocks it, so always run both; then start Creo.
`config.pro` needs `protkdat C:\dev\Toolkit-PDM\protk.dat`. Customize Ribbon -> "CreoPDM Structure Export" for the icon.
Log: `%TEMP%\creopdm_toolkit.log` (renamed from `creopdm_structure.log`). Look for `HTTP listening on 127.0.0.1:8767`.

## Test matrix (Creo open, aux app loaded)
```powershell
$b = "http://127.0.0.1:8767"
Invoke-RestMethod "$b/health"
```
| Active model | Calls |
|---|---|
| Part | `/identity` `/units` `/parameters` `/mass` `/materials` `/features` `/relations`; `/collect?domains=mass,features`; `/structure` must say "applies to ASSEMBLY/MFG" |
| Assembly | `/structure` `/structure?recurse=1` `/mass` `/features` `/constraints` `/simp-reps` `/relations`; `/collect` |
| Drawing | `/identity` `/drawing` `/features` `/parameters`; `/mass` must say "applies to PART/ASSEMBLY" |

```powershell
(Invoke-RestMethod "$b/collect").domains.PSObject.Properties.Name
Invoke-RestMethod "$b/structure?recurse=1" | ConvertTo-Json -Depth 6
```
`curl.exe http://127.0.0.1:8767/mass` works too.

Sample `/structure` output: [samples/tdd-demo.structure.json](samples/tdd-demo.structure.json) (real, from the pilot).

## Revision notes (live-test fixes)
- **Assembly `/mass`**: tries `ProSolidMassPropertyGet`, `ProAssemblySolidMassPropertyGet`, `ProSolidMassPropertyWithDensityGet`; if all
  fail it sums leaf parts **already in session** (mass, volume, area, mass-weighted COG via `ProAsmcomppathTrfGet`; `method:"component_sum"`,
  `principal_moments:null`). It only returns a sum when every non-suppressed part resolved; otherwise
  `ok:false "assembly mass unavailable (<each rc>; component sum: N with mass, M without density, K unresolved)"`. `method` says which path won.
- **`/constraints`**: full `ProAsmcompConstrType` decode (DEFAULT, FIX, AUTO, SUBSTITUTE, angle/distance types, ...), raw `type_id` for diagnosis,
  per-component `packaged`. `offset_units` = owner assembly length unit (or `deg` for angular types). Rows with `null` references are
  expected for DEFAULT/FIX placements. If any row still shows `UNKNOWN`, send its `type_id`.
- **`/simp-reps`**: no sentinel ids (see above).
- **`/drawing`**: unique string ids. **`/collect` on a drawing** omits `units` (not applicable) and `drawing`; `features` carries the drawing
  inventory (for DRAWING `/features` is an alias of `/drawing`, and an explicit `?domains=units` returns a clear not-applicable error).
- **`/identity`**: `model_role` = `SKELETON`/`SHEETMETAL` when Toolkit reports it, plus a `subtype` label. **`/features`** type names are
  normalized to `DATUM_PLANE` style. **`/relations`** envelope unchanged (`normal` / `post_regeneration` / `features` / `errors`).

## Drawing inventory (v0.3.x)
Row fields: `id` (string, unique), `creo_id`, `name`, `type` (SHEET|VIEW|NOTE|DIMENSION|TABLE), `subtype`, `level`, `status`, `suppressed`,
`sheet`, `sheet_id`, `parent_id`. Levels: sheet 1; view/note/table 2; dimension 3 when it belongs to a view, else 2.
Extras: NOTE `text`; DIMENSION `value`, `units`, `symbol`, `owner`, `view_id`, `reference`; TABLE `rows`, `columns`, `segments` (+ `cells` with `?cells=1`).

`/collect` on a DRAWING returns `identity`, `parameters`, `drawing` (canonical, same payload as `GET /drawing`). `features` is **omitted** from the
default collect for drawings (no duplicate payload); an explicit `?domains=features` or `GET /features` still returns the same inventory as an alias.
`units`, `mass`, `structure`, `constraints`, `simp_reps`, `relations`, `materials` are not collected for drawings.

## Drawing dimension units (policy)
Values are never converted; `units` names the unit the value is stored in. **Lengths always use a MODEL length unit, never the drawing sheet unit.**
| Dimension | `units` | `units_basis` | `units_model` |
|---|---|---|---|
| ANGLE | `deg` | `angle` | null |
| Solid-owned (shown from a model) | owner model length unit (e.g. `mm`) | `owner_model` | the owner model |
| Drawing-created | length unit of the model it measures: its attachment's model, else its view's model, else the first model shown in the drawing | `owner_model` | that model |

`units_basis:"unresolved"` (units null) means no model could be found. `value` is `ProDimensionValueGet` as returned; `value_displayed` is
`ProDimensionDisplayedValueGet` (rounded, as shown) for cross-checking. `subtype` = LINEAR / RADIUS / DIAMETER / ANGLE / ARC_LENGTH from
`ProDimensionTypeGet`, with the raw `type_id`; other values give `OTHER`, a failed call gives `UNKNOWN`, both with `units_assumed:true`.
Ordinate detection is not attempted (the API needs an extra license option). No model is retrieved.

## MFG support
| Endpoint | MFG |
|---|---|
| `/health`, `/model`, `/identity`, `/parameters`, `/units` | yes (`/model` -> `{ok,fileName,type,model:{fileName,type}}` for any model type) |
| `/mfg` | yes: `{ok,fileName,mfg_type,partial,partial_missing[],modelCount,featureCount,skippedNoModel,models[],features[]}` |
| `/structure`, `/constraints`, `/features`, `/mass`, `/materials`, `/relations`, `/simp-reps` | rejected with a clear message (`... (use /mfg)` where relevant); **no solid feature walk is ever run on an MFG handle** (an MFG handle is not a ProSolid; that call returned -2) |

`/mfg` reads session data only (nothing retrieved):
- `models[]`: components of the MFG top-level assembly (`ProMfgAssemGet`), ids `mfgmdl:N`, `role` = REF_MODEL | WORKPIECE | FIXTURE (`ProAsmcompTypeGet`), plus type/status/suppressed.
- `features[]`: features of the MFG storage solid (`ProMfgSolidGet`): ids `mfgop:N` (operations), `mfgwc:N` (workcells), `mfgfix:N` (setups/fixtures), `mfgseq:N` (NC sequences),
  `mfgfeat:N` (other). `type` = that kind, `subtype` = Toolkit feature type name, `detail` = Toolkit feature subtype, `creo_id` numeric. Datum/csys/curve features are omitted.
- `partial:true` + `partial_missing` (`models` and/or `operations`) if either subtree cannot be read.
- Limits: NC sequences are recognised by feature-type-name keywords (Toolkit has no single NC-sequence feature type), so unusual sequence types may show as `OTHER`
  (the `subtype` field tells you what they are); operation -> sequence nesting (`level`) is not derived; tools, parameters of sequences and cl-data are not read.
- `/identity` for MFG still reports whatever `ProMdlCommonnameGet`/`ProMdlOriginGet` return; if `common_name`/`origin` point at the related .asm, that is Toolkit's answer, not a lookup bug here.
`/collect` on an MFG: `identity`, `units`, `parameters`, `mfg`.

### MFG polish (v0.3.x)
- `features[]` never contains COMPONENT features (they are in `models[]`).
- Stock models: `type:"STOCK_MODEL"`, ids `mfgstock:N`, `subtype` = Toolkit feature type name.
- Nesting: OPERATION / WORKCELL / SETUP / FIXTURE / STOCK_MODEL are `level:1`. An NC sequence is `level:2` with `parent_id:"mfgop:N"` when `ProFeatureParentsGet`
  lists an operation among its parents. Parents are never guessed from visit order; if none is found `level` and `parent_id` are null and `"nesting"` is added to
  `partial_missing` (`partial:true`).
- `/identity` for MFG: `common_name` = the MFG file name and `origin` = the folder of Toolkit's origin + the MFG file name (`origin_derived:true`; the `.N` version is
  not available from Toolkit). Toolkit's own values (the related assembly) are kept in `identity.related_assembly {common_name, origin}`.

### MFG nesting rule (all MFG geometry/sequence features)
Applies to NC sequences **and** every other non-top-level MFG feature (mill windows/volumes/surfaces, drill groups, ...). Operations, workcells, setups/fixtures and stock are level 1.
Owner resolution, in order, using Toolkit links only (no visit-order or "first operation" guessing):
1. `operation_reference`: an operation is among the feature's direct parents (`ProFeatureParentsGet`) -> `level:2`, `parent_id:"mfgop:N"`.
2. `referenced_by_sequence`: no direct operation, but the features that depend on it (`ProFeatureChildrenGet`, followed up to 4 levels, e.g. window -> volume -> sequence) all belong to the SAME operation -> nested under it.
   Used by several operations, or used by none -> left unowned.
Unowned features keep `level`/`parent_id`/`parent_basis` null and `"nesting"` stays in `partial_missing` (with `partial:true`); it is removed once nothing is unowned. Features that are
not geometry-related but still unowned (e.g. a stray `OTHER` feature) also count, so `nesting` can remain listed even when all windows are parented; check which rows have `level:null`.
