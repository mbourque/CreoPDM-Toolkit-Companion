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

## Collecting a model other than the active one (no windows)
Every domain endpoint and `/collect` accept `?fileName=<name.ext[.N]>&path=<absolute path to the file>` (URL-encode the path). Without `fileName` the active model is used.
- `fileName` already in session -> used as is (nothing opened, nothing erased).
- Not in session -> retrieved **silently** with `ProMdlFiletypeLoad(path, type, ask_user_about_reps=FALSE)`: the model is loaded into memory but **not displayed, no window/tab is created, and it does not
  become the current model**. `path` may be the file or its folder. A missing `path` for a model that is not in session returns `{ok:false,"... is not in session; pass path= ..."}` (never a windowed open).
- After the request, a model **this call retrieved** is erased with `ProMdlErase` (that single model only; never `ProMdlEraseAll`, never anything that was already in session, never the current model).
  Components pulled in with an assembly stay in session, undisplayed. `?keep=1` leaves the retrieved model in session as well.
- Example: `Invoke-RestMethod "http://127.0.0.1:8767/collect?domains=identity,features&fileName=base-plate.prt&path=C%3A%5Cmodels%5Cbase-plate.prt.4"`
- Not yet verified in Creo: run it for 20+ different parts and confirm the window/tab count does not grow.

### `/structure` row shape (Creo.JS Structure rows)
`{id, name, featureName, type, subtype, level, status, suppressed, path}` (+ `expanded` with `?recurse=1`). `name` is always the model tip with extension (`shaft.prt`, `sub.asm`, lowercase, no `.N`);
`type` is PART | ASSEMBLY | SKELETON (never COMPONENT); `subtype` is `""`; `path` is the component id path (`"46"` top level, `"40/669"` nested). Only real components are walked,
so datums, CSYS and cross-sections cannot appear (they are in `/features`). In `/mfg`, `models[]` rows use the same `name`/`type` plus `role`.

## Collect contract (summary)
`GET|POST /collect?domains=a,b,c[&fileName=<name.ext[.N]>&path=<abs path>][&keep=1][&recurse=0|1][&cells=1]`
- **Target:** `fileName` (+ `path`) picks the model; without it the active model is used. In session -> used as is. Not in session -> ONE silent retrieve (`ProMdlFiletypeLoad`, no display/window). No `path` and not in session -> `ok:false`.
- **One retrieve per request:** all requested domains run on that single in-session model; nothing is retrieved or erased between domains. (Separate HTTP calls for the same file are separate requests; use one `/collect` with all domains instead, or `keep=1`.)
- **`domains`:** comma list (`identity,parameters,features,materials,mass,units,...`); omitted/`all` = every domain that applies to the model type.
- **Erase:** default = after the request, a model THIS call retrieved is erased (`ProMdlErase`, that model only; never one already in session, never the current model). **`keep=1`** skips the erase, so the model stays in session (undisplayed) and later calls for it reuse it with no reload (`retrieved:false`, `retrieve_ms` ~0). The caller owns cleanup of kept models (or leave them). Components pulled in with an assembly always stay in session.
- **Single-threaded / queued:** all Toolkit work runs on Creo's main thread and requests are handled strictly one at a time; concurrent callers queue (30 s per-request timeout, `CREOPDM_TOOLKIT_TIMEOUT_MS`).
- **`timings_ms`** is appended to every `/collect` and single-domain response: `{retrieved, retrieve_ms, <domain>_ms for each domain that ran, erase_ms, total_ms}`. The same line is written to `%TEMP%\creopdm_toolkit.log`, e.g.
  `/collect base-plate.prt retrieved=1 retrieve=842ms identity=1ms features=6ms mass=3ms erase=2ms total=861ms`.

### Hidden / unnamed features
`/features`, the per-feature rows of `/relations`, and `/mfg` features skip **internal (invisible) features** (`ProFeatureVisibilityGet` = FALSE; `ProSolidFeatVisit` visits them otherwise) and
any feature whose raw name is Creo's `no_name` placeholder (case-insensitive). The literal `no_name` is never emitted: a feature-relations row with such a name has `feature_name: null`.
Unnamed but *visible* features still appear with the `TYPE (id)` fallback name.

### `/structure` is now a full nested tree by default
`recurse` now defaults to **1** (use `?recurse=0` for top-level only). Levels: top-level components 1, children of a sub-assembly 2, and so on; `path` is the id chain from the root.
Only **component features** are walked (one `ProSolidFeatVisit` per assembly); no features/parameters/materials/mass of children are read and **nothing is retrieved**.
A sub-assembly is expanded only if its model is already in memory (always true after the root assembly was retrieved/loaded, since Creo loads its components, or after parts-first + `keep=1`).
A non-suppressed sub-assembly that is not in memory is listed by name only with `expanded:false`; the response then has `partial:true` and `unexpandedSubassemblies:N`. Suppressed sub-assemblies are never expanded.
The existing domain name (`structure`) and URL are unchanged, so the agent needs no new parameter.

### Skeleton typing in `/structure`
A component is a skeleton when `ProMdlIsSkeleton` says so for its model (if in memory), or, for name-only components, when it matches the owner assembly's primary skeleton (`ProAsmSkeletonGet`).
Rows then have `type:"SKELETON"`, `subtype:"SKELETON"` and `is_skeleton:true`. Other parts: `type:"PART"`, `is_skeleton:false`. If a component model is not in memory and is not the owner's
primary skeleton, `is_skeleton` is `null` (unknown, not "false") and `type` is `PART`; in that case `type` is a best guess. Assemblies are always `is_skeleton:false`.

### Identity hardening
- `identity.file_name` is always the logical tip name (`shaft.prt`, lowercase) from the model handle; if the handle returns no name it falls back to the requested `fileName=` as given (e.g. `shaft.prt.2`).
- If the model cannot be retrieved: `{ok:false,error:"could not retrieve <file> ..."}` (no identity at all). If the handle is unusable: `ok:false "model handle for <file> is not usable"`.
- If there is still no name, `/identity` returns `ok:false "identity unavailable: model has no usable file name"`; it never returns `ok:true` with an empty identity.

## Expected skips / stable error strings
Collect with `fileName=`+`path=` returns `{ok:false,error:"..."}` with these prefixes (safe to treat as expected skips):
| Error prefix | Meaning |
|---|---|
| `harness_not_openable_in_isolation: open the cabling assembly that contains <tip>` | The file is a cabling harness part. Creo cannot open it standalone, so no load is attempted. |
| `<tip> is not in session; pass path= ...` | Model not in session and no `path`. |
| `file_not_found: <path> (ProMdlFiletypeLoad=N)` | The path does not exist. |
| `could not retrieve <tip> from path (ProMdlFiletypeLoad=N)` | Any other load failure. |

Harness detection: Toolkit has no API to inspect a model without loading it (a `ProHarness` is just a loaded part), so the file's first line is read (<=200 bytes) before any load: Creo-saved harness parts start with
`#UGC:2 PART/HARNESS_PART ...` (sheetmetal parts start `PART/SHEETMETAL`, normal parts `PART`). The check runs only when the model is NOT already in session; if the harness was opened through its cabling
assembly it is in session and collects normally. The same check is repeated after a failed load. Verified against Creo's own `striped_cable/_striped_cable_preview_harness.prt`; if a harness from another Creo version
is not recognised it falls through to the generic `could not retrieve ... =-1` error.

### Harness tips: identity succeeds, everything else is skipped
For a harness tip that is not in session, `identity` is answered from the file header alone (no `ProMdlFiletypeLoad`, nothing retrieved):
```json
GET /identity?fileName=600640041454.prt&path=C%3A%5C...%5C600640041454.prt.3
{"ok":true,"fileName":"600640041454.prt",
 "identity":{"file_name":"600640041454.prt","model_type":"PART","model_role":"HARNESS","subtype":"HARNESS",
             "origin":"C:\...\600640041454.prt.3","identity_source":"file_header"}}
```
`file_name` is the requested tip (lowercased, as given incl. any `.N`). `identity_source:"file_header"` marks it as not read from a loaded model.

`/collect` for a harness tip is `ok:true` with `"harness":true`, `"type":"PART"` and a per-domain result, so CreoPDM can save identity and skip the rest:
```json
GET /collect?fileName=600640041454.prt&path=...&domains=identity,parameters,features
{"ok":true,"fileName":"600640041454.prt","type":"PART","harness":true,
 "domains":{"identity":{"ok":true,"fileName":"...","identity":{...as above...}},
            "parameters":{"ok":false,"error":"harness_not_openable_in_isolation: open the cabling assembly that contains 600640041454.prt"},
            "features":{"ok":false,"error":"harness_not_openable_in_isolation: ..."}}}
```
With no `domains` (or `all`) the same applies to every domain a PART collect would run. Domain behavior for a harness tip not in session:
| Endpoint / domain | Result |
|---|---|
| `identity` | `ok:true` (HARNESS stub above) |
| `parameters`, `features`, `materials`, `mass`, `units`, `relations`, any other single-domain endpoint | `ok:false` `harness_not_openable_in_isolation: ...` |
Units are not offered for harnesses: they need a loaded model. A harness already in session (opened through its cabling assembly) is collected normally with real data. Harness responses also carry `timings_ms` (retrieve ~0).

### Harness members in `/structure`
Harness components are always listed, never retrieved:
- The component name is resolved without loading (`ProAsmcompMdlGet` -> `ProAsmcompMdlMdlnameGet` -> `ProAsmcompMdldataGet`), so a harness with no in-memory model keeps its tip name.
- A component is marked harness when it is in the owner assembly's Toolkit harness list (`ProAssemblyHarnessesTopCollect`) or, for an in-memory model, its on-disk header says `HARNESS` (same `#UGC:` check used for `harness_not_openable_in_isolation`).
- A **supplement pass** per assembly (including sub-assemblies) adds any harness from that list the component visit did not report. If no component feature references it, the row has `id:null`.
Row example:
```json
{"id":612,"name":"600640041454.prt","featureName":null,"type":"HARNESS","subtype":"HARNESS",
 "is_skeleton":false,"is_harness":true,"level":1,"status":"ACTIVE","suppressed":false,"path":"612","expanded":false}
```
Other rows have `is_harness:false`. HARNESS rows are counted in `componentCount`. `/constraints` skips a harness that has no component feature. A harness still counts in the component-sum assembly mass if its model is in memory.
Unverified in Creo: if harness members are neither `PRO_FEAT_COMPONENT` features nor in Toolkit's harness list for their owner assembly, they will still be missing; send the raw `/structure` JSON in that case.

## `bom` domain (assemblies)
`GET /bom` or `/collect?domains=bom` (ASSEMBLY only). It is **not** in the default `/collect` set; request it explicitly. `structure` is unchanged and separate.
- Rolled up per assembly level by **logical tip name** (`shaft.prt`, never `shaft.prt.2`). `quantity` = non-suppressed occurrences under ONE instance of the parent (children are not multiplied by the parent's quantity). Suppressed components are excluded (`suppressedExcluded` counts them).
- Each unique sub-assembly is expanded once if its model is already in session (same keep=1 / parts-first pattern); **nothing is retrieved**. Depth limit 6.
- A sub-assembly that cannot be expanded is a leaf with `"expanded":false` and `"leaf_reason":"not_in_memory"` (or `"max_depth"`); the response then has `partial:true`, `unexpandedNotInMemory` / `unexpandedMaxDepth`.
- Node: `{filename, quantity, dependency_type: ASSEMBLY_ROOT|ASSEMBLY_MEMBER, type: PART|ASSEMBLY|SKELETON|HARNESS, children[], expanded?, leaf_reason?}`. Harness members are listed with `type:"HARNESS"` and a quantity, never loaded. Skeletons are listed with `type:"SKELETON"` so CreoPDM can filter them if its BOM should omit them.
- `bom` is a single-element array holding the `ASSEMBLY_ROOT` wrapper (matches `CreoMetadataRequest.bom` as a list of nodes).

Illustrative sample (small nested assembly `main.asm`: 1 sub-assembly used twice, 4 screws, 1 harness; the sub-assembly's own children listed once):
```json
{"ok":true,"fileName":"main.asm","partial":false,"unexpandedNotInMemory":0,"unexpandedMaxDepth":0,"suppressedExcluded":0,"maxDepth":6,
 "bom":[{"filename":"main.asm","quantity":1,"dependency_type":"ASSEMBLY_ROOT","type":"ASSEMBLY","expanded":true,"children":[
   {"filename":"base-plate.prt","quantity":1,"dependency_type":"ASSEMBLY_MEMBER","type":"PART","children":[]},
   {"filename":"bracket.asm","quantity":2,"dependency_type":"ASSEMBLY_MEMBER","type":"ASSEMBLY","expanded":true,"children":[
      {"filename":"arm.prt","quantity":1,"dependency_type":"ASSEMBLY_MEMBER","type":"PART","children":[]},
      {"filename":"bushing.prt","quantity":2,"dependency_type":"ASSEMBLY_MEMBER","type":"PART","children":[]}]},
   {"filename":"iso4762-m10x30.prt","quantity":4,"dependency_type":"ASSEMBLY_MEMBER","type":"PART","children":[]},
   {"filename":"600640041454.prt","quantity":1,"dependency_type":"ASSEMBLY_MEMBER","type":"HARNESS","children":[]}]}]}
```
(Sample is hand-written to show the shape; it is not output captured from Creo.)

### `/features`: groups nest their members
Group membership comes from `ProFeatureGroupGet` (the group head that contains a feature). Rows keep the existing shape `{id,name,type,subtype,level,status,suppressed}`; `level` is now filled:
top-level features are `level:1`, a `GROUP_HEAD` stays at its own level and is followed by its members at `level+1` (nested groups go deeper). Previously `level` was `null` for every row.
A member whose group head is not in the list (e.g. hidden/internal) is shown at level 1, so no feature is lost. Applies to ordinary groups and cabling groups alike.
```json
[{"id":40,"name":"DEFAULT_CSYS","type":"COORDINATE_SYSTEM","subtype":"","level":1,"status":"ACTIVE","suppressed":false},
 {"id":210,"name":"BOLTS","type":"GROUP_HEAD","subtype":"","level":1,"status":"ACTIVE","suppressed":false},
 {"id":214,"name":"BOLT_1","type":"COMPONENT","subtype":"","level":2,"status":"ACTIVE","suppressed":false},
 {"id":216,"name":"BOLT_2","type":"COMPONENT","subtype":"","level":2,"status":"ACTIVE","suppressed":false},
 {"id":230,"name":"HARNESS-ACTUATORS","type":"GROUP_HEAD","subtype":"","level":1,"status":"ACTIVE","suppressed":false},
 {"id":234,"name":"SPOOL_1","type":"SPOOL","subtype":"","level":2,"status":"ACTIVE","suppressed":false}]
```
(Illustrative shape; not captured output.)

### Harness discovery (`/harness-probe`) and the wider harness list
Harnesses are listed in `/structure` (and `/bom`) from: (1) component features whose model is a harness (in the owner assembly's `ProAssemblyHarnessesTopCollect` list, or on-disk header `HARNESS`), (2) the owner's harness list
for any harness no component feature references (`id:null`), and (3) with `recurse` on, a root-level pass over `ProAssemblyHarnessesCollect` (the assembly **and its sub-assemblies**) for any harness not yet reported
(listed at level 1, no owner path). Nothing is loaded.

`GET /harness-probe` (assembly only, diagnostic, not part of the default `/collect`; also `domains=harness_probe`) dumps what Toolkit knows, to find out how a given assembly represents its harness:
`ProAssemblyHarnessesTopCollect` / `ProAssemblyHarnessesCollect` return codes and model names, spools, connector count, every `GROUP_HEAD` with its members (feature id/type/name and, for component members, the model name and
whether it is in memory), and all cabling-type features (cable, cable segment, sub-harness, ribbon cable). If a harness tip is not in `harnessesTop`/`harnessesAll` and not a component member of a group, Toolkit does not expose it as a
model for that assembly, and `/structure`/`/bom` cannot list it.
