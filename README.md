# CreoPDM Toolkit Companion

A Creo Parametric auxiliary application (Pro/TOOLKIT, C) that reads metadata out of Creo and serves it over a small **localhost HTTP API**.

> **This is a companion to, and a requirement for, [CreoPDM](https://github.com/mbourque/CreoPDM).** CreoPDM's agent calls this service on `127.0.0.1` during **Collect** to read identity, parameters, features, structure, BOM, mass, drawings and manufacturing data from Creo models. Install it into every Creo session that CreoPDM Collect will use.

- **Windows x64**, Creo Parametric **10 and later**. Built against the Creo 10 SDK; tested by the author in Creo 10 through 13.
- The app lives **inside a running Creo** (a Toolkit app cannot run standalone). It listens on `127.0.0.1` only and never opens windows or tabs to collect.
- A prebuilt, unlocked `bin\creopdm_structure.dll` is included, so you can use it without building. (The DLL keeps its original name, `creopdm_structure.dll`, for compatibility with existing installs.)

## Contents
[Install](#install-into-creo) · [Check it works](#check-that-it-works) · [Troubleshooting](#troubleshooting) · [Build from source](#build-from-source-optional) · [HTTP API](#http-api) · [Collect contract](#collect-contract) · [Domains in detail](#domains-in-detail) · [Error strings](#expected-skips--error-strings) · [Developer notes](#developer-notes)

---

## Install into Creo
**You need three things**, kept together in one permanent folder (all three are in this repo):

| Item | What it is |
|---|---|
| `bin\creopdm_structure.dll` | The app (prebuilt and unlocked). |
| `protk.dat` | The registration file Creo reads. It points at the DLL and the text folder. |
| `text\usascii\creopdm_msg.txt` | Message strings (ribbon label, the startup message line). |

**1. Download** this repo from <https://github.com/mbourque/CreoPDM-Toolkit-Companion> (Code > Download ZIP, or `git clone https://github.com/mbourque/CreoPDM-Toolkit-Companion.git`) and put the three items somewhere permanent: outside Creo's own install folder, and not on a drive that can be offline. For example `C:\CreoPDM\Toolkit\`:
```
C:\CreoPDM\Toolkit\
    protk.dat
    bin\creopdm_structure.dll
    text\usascii\creopdm_msg.txt
```
(Running straight from the cloned repo folder also works; just use that folder's paths below.)

**2. Edit `protk.dat`** so the two paths match where you put the files. The copy in this repo has the developer's paths (`C:\dev\Toolkit-PDM\...`), so it must be changed:
```
NAME        CreoPDMStructure
EXEC_FILE   C:\CreoPDM\Toolkit\bin\creopdm_structure.dll
TEXT_DIR    C:\CreoPDM\Toolkit\text
STARTUP     dll
ALLOW_STOP  TRUE
REVISION    18
END
```
Leave `REVISION 18` as is unless Creo reports a revision problem when loading.

**3. Add one line to `config.pro`**, using the full path to your `protk.dat`:
```
protkdat C:\CreoPDM\Toolkit\protk.dat
```
Put it in a `config.pro` that Creo reads at startup: the one in Creo's startup (working) folder for a single user, or the system `config.pro` in `<Creo install>\Common Files\text\` for everyone on the machine. If you already register other Toolkit apps, add this as another `protkdat` line (Creo accepts several).

**4. Restart Creo.** `config.pro` is only read at startup.

If you copy a newer `creopdm_structure.dll` over an old one later, close Creo first (it locks the DLL).

### Check that it works
- On startup the message line shows *"CreoPDM Structure: HTTP listening on 127.0.0.1."* The app also appears in Creo's Auxiliary Applications list.
- From PowerShell, with Creo running:
  ```powershell
  Invoke-RestMethod http://127.0.0.1:8767/health
  ```
  should return `{"ok": true, "service": "creopdm-toolkit", "version": "...", "model": ...}` (`model` is the active model, or `null` if none is open).
- Log file: `%TEMP%\creopdm_toolkit.log`.

**Settings** are environment variables, set before Creo starts:

| Variable | Default | Meaning |
|---|---|---|
| `CREOPDM_TOOLKIT_PORT` | `8767` | Port on `127.0.0.1` (the older name `CREOPDM_STRUCTURE_PORT` is still honoured). |
| `CREOPDM_TOOLKIT_TIMEOUT_MS` | `30000` | How long a request waits for Creo's main thread. |
| `CREOPDM_URL` | (none) | Address of your CreoPDM web UI, used by the experimental **CreoPDM** and **CreoPDM Browse** commands (see [ribbon tab](#creopdm-ribbon-tab-experimental)). |
| `CREOPDM_EXPLORER_URL` | `CREOPDM_URL` + `embed/explorer` | Address of the compact file-explorer page shown by **CreoPDM Browse**. |
| `CREOPDM_BROWSER_PERCENT` | `50` | How much of the Creo window the embedded browser covers after the **CreoPDM** command (10 to 95). |

### Troubleshooting
| Symptom | Likely cause / fix |
|---|---|
| Creo says the app "was not unlocked before distribution", or "Startup of application … failed" | The DLL is not unlocked (a locally rebuilt DLL must be unlocked, see [Build](#build-from-source-optional)). Also check the DLL path in `protk.dat`. |
| Nothing happens at startup, no message line | Check the `protkdat` line path, the `EXEC_FILE` / `TEXT_DIR` paths in `protk.dat`, and that the `config.pro` you edited is one Creo actually reads. |
| Message line says HTTP not started | Port 8767 is in use (see the log). Set `CREOPDM_TOOLKIT_PORT` to another port. |
| `/health` works but a model call returns `ProMdlFiletypeLoad=-1` | Creo could not open the file. For example it was saved in a newer Creo release than the one running (an older Creo cannot open newer files). |
| Replacing or rebuilding the DLL fails with `cannot open file 'bin\creopdm_structure.dll'` | Creo has the DLL loaded; close Creo first. |

---

## CreoPDM ribbon tab (experimental)
The app registers commands you can place in a **CreoPDM** tab of Creo's ribbon:

| Command | Does today |
|---|---|
| **CreoPDM** | Shows the full CreoPDM web UI in Creo's **embedded browser** and sets how much of the Creo window it covers (`CREOPDM_BROWSER_PERCENT`, default 50). The address comes from the `CREOPDM_URL` environment variable (set it before starting Creo, e.g. `http://localhost:8000/`). If it is not set, the message line says so. |
| **CreoPDM Browse** | Shows a compact **file explorer for the current product** in a Creo **navigator pane** (the narrow panel beside the model tree). The page is `CREOPDM_EXPLORER_URL`, or `CREOPDM_URL` + `embed/explorer`; the active model is passed as context (`?file=<name>&folder=<folder>`) so CreoPDM can choose the product. This needs that explorer page to exist in CreoPDM (CreoPDM provides `/embed/explorer`). A second click reloads the pane with the current model as context. Opening a file from that page goes through the CreoPDM agent and the `/open` call below; the browser never calls this service directly. |

This is a first experiment. Both commands write their result codes to the log file creopdm_toolkit.log in your TEMP folder (`ProWindowURLShow` / `ProWindowBrowserSizeSet` for CreoPDM; `ProNavigatorpaneBrowserAdd` / `ProNavigatorpaneBrowserURLSet` for Browse). The width setting applies to the embedded browser only, not to the navigator pane (Creo has no control for the navigator's width), and not if Creo shows its browser in a separate window. After adding a new command, put it in the tab (step 3 below) and save the layout again.

**Lay out the tab** (Creo's standard Toolkit ribbon workflow; needs a Toolkit development license to save):
1. Start Creo with the app loaded, open the mode you want the tab in (Part, Assembly, Drawing, ...).
2. File > Options > **Customize Ribbon**. In the "Customize the Ribbon" list create a **new tab** named CreoPDM and a group in it.
3. Set "Choose commands from" to **TOOLKIT Commands**, select the CreoPDM commands and **Add** them to the group.
4. **Import/Export > Save the Auxiliary Application User Interface.** Creo writes `toolkitribbonui.rbn` into the app's `text\` folder (the `TEXT_DIR` in `protk.dat`).
5. Click Apply/OK and restart Creo. The tab loads automatically together with the app.

Notes: the layout is saved **per Creo mode** (repeat in each mode where the tab should show); only new tabs and groups can be added, not Creo's own; the `.rbn` file is generated by Creo and cannot be edited by hand. Commit the saved `toolkitribbonui.rbn` (it sits beside `text\usascii`) and it ships with the app. A file with another name can be placed in `text\ribbon\` and loaded by the app with `ProRibbonDefinitionfileLoad`.

---

## Build from source (optional)
You only need this to change the code. Requirements: Visual Studio 2022 (x64 tools), the **Creo API Toolkit** option installed (headers and libs under `<Creo>\Common Files\protoolkit`), and a Toolkit license to unlock the result.

```bat
build.bat
unlock.bat
```
- Close Creo first (it locks the DLL). **A rebuild is locked again, so always run both**; then start Creo.
- To build while Creo is still running, build a separate copy: `set OUT_DLL=bin\creopdm_structure.new.dll`, run `build.bat` and `unlock.bat` in the same prompt, close Creo, then copy that file over `bin\creopdm_structure.dll` (and delete the `.new` files).
- Both scripts use `%CREO_HOME%` (default `C:\PTC\Creo 10.0.10.0`). To build against another install: `set CREO_HOME=C:\PTC\Creo 13.4.1.0`. If Visual Studio lives elsewhere, edit `VCVARS` at the top of `build.bat`.
- **Target the oldest release you need.** A DLL built against an older SDK is meant to load in that release and later (not the reverse). SDK differences are handled at compile time: drawing tables use `ProTable` when `ProTable.h` exists (Creo 11+ SDKs) and the older `ProDwgtable*` calls otherwise; one constraint label is matched by value. A Creo 10 SDK build therefore uses the older table calls everywhere, including when run in Creo 13 (deprecated there but present).
- If a newer Creo reports the app "was not unlocked before distribution", run that release's `protk_unlock.bat` on the same DLL (safe to repeat on an already-unlocked DLL).

---

## HTTP API
The service binds **127.0.0.1 only**. `GET` and `POST` are equivalent. Every response is JSON; failures are HTTP 200 with `{"ok": false, "error": "..."}` (404 unknown path, 405 bad method). `simp-reps` and `simp_reps` are interchangeable.

| Endpoint | PART | ASM | MFG | DRW | Response (inner key = what CreoPDM stores) |
|---|:-:|:-:|:-:|:-:|---|
| `/health` | x | x | x | x | `{ok, service:"creopdm-toolkit", version, model:{fileName,type}\|null}` (`busy:true` if Creo is busy) |
| `/model` | x | x | x | x | `{ok, fileName, type, model:{fileName,type}}` |
| `/identity` | x | x | x | x | `{ok, fileName, identity:{file_name, common_name, full_name, generic_name, instance_name, origin, model_type, model_role, subtype, is_modified, version_stamp}}` |
| `/units` | x | x | x | | `{ok, fileName, units:{system_name, length, mass, time, temperature, angle}}` |
| `/parameters` | x | x | x | x | `{ok, fileName, parameters:[{name, value, data_type, units, description, is_designated}]}` |
| `/mass` | x | x | | | `{ok, fileName, mass:{mass, volume, surface_area, density, gravity_center[3], principal_moments[3], cg_inertia_tensor[3][3]}, method}` |
| `/materials` | x | | | | `{ok, fileName, materials:{current, names[]}}` |
| `/features` | x | x | | x | `{ok, fileName, features:[{id, name, type, subtype, level, status, suppressed}]}`; for a drawing this is the same inventory as `/drawing` |
| `/dimensions` | x | x | | | `{ok, fileName, dimensionCount, dimensions:[{symbol, value, value_displayed, units, type, feature_id, feature_name, feature_type}]}`. Model (feature) dimensions only, not drawing annotations. `feature_id` is the id `/features` uses (e.g. 67 for CHAMFER (67)). `units` is the model length unit, `deg` for angles. Features `/features` does not list (invisible, `no_name`, components) are skipped. `?reference=1` adds reference dimensions (`reference:true`). Part of the default `/collect` for PART/ASSEMBLY. |
| `/sketch` | | | | | **2D section (`.sec`) only.** `{ok, units:{length,angle}, outline:{min,max,width,height}, entities:{total, counts{line,arc,circle,spline,point,centerline,text,other}, construction_count, items[], truncated}, dimensions:[{id:"sd0", type, value, display, entity_ids, reference:null, tolerance:null}], constraints:[{type, entity_ids}], loops:{closed,open}, status{...}}`. `/collect?fileName=x.sec&path=...` returns `identity` + `sketch` (other domains answer "does not apply to a SECTION"). `items` is capped at 2000 (`truncated:true`); counts, dimensions, constraints and loops are always complete. Arc angles are in degrees; lengths in the section's unit. The section is only read (never regenerated). Fields Toolkit cannot supply are `null` (`reference`, `tolerance`, `fully_dimensioned`, `over_constrained`, strong/weak counts, `regenerated`). Denied (disabled) constraints are not listed. `loops` are derived from curve end points (closed = every end shared by two curves; circles count as closed). Identity for a section adds `creo_release` (running Creo) and `saved_release` (from the file header). On Creo 13 sections `ProSectionEntityIdsGet` and `ProSectionEntityGet` return BAD_INPUTS, so entity ids are found by probing `ProSectionEntityEval` over 1..N (N from the ids dimensions/constraints reference) and geometry is rebuilt from 9 evaluated points per entity: type is inferred (point, line, circle, arc, else spline with 9 sampled points); centerline, point and text cannot be told apart, and `units.length` is null (Toolkit gives no unit system for a section); `notes` says so (informational, not a failure); `partial_missing` lists only data that really could not be read. The section is never regenerated. A part that cannot be read is `null` and named in `partial_missing` (function, return code, entity/dimension id); only when entities, dimensions and constraints all fail is the domain `ok:false`. `identity.subtype` is `null` (not `"NONE"`) when a model has none. `?domains=` is URL-decoded (`identity%2csketch` works). Test sample: `...\CreoPDM-agent\workspaces\aeda749f-739a-483a-bf28-fed7a86ee20b\test-sec.sec.1` (23 KB, lines + dimensions). |
| `/relations` | x | x | | | `{ok, fileName, normal[], post_regeneration[], features:[{feature_id, feature_name, relations[]}], errors[]}` |
| `/structure` | | x | | | `{ok, fileName, componentCount, skippedNoModel, recurse, unexpandedSubassemblies, partial, components:[...]}`; `?recurse=0` for top level only |
| `/bom` | | x | | | `{ok, fileName, partial, ..., bom:[ASSEMBLY_ROOT node]}` |
| `/constraints` | | x | | | `{ok, fileName, recurse, components:[{id, name, path, level, suppressed, packaged, constraints[]}], errors[]}` |
| `/simp-reps` | | x | | | `{ok, fileName, active:{id, name, is_master}, representations:[{id, name, is_active}], errors[]}` |
| `/drawing` | | | | x | `{ok, fileName, partial, partial_missing[], sheetCount, viewCount, noteCount, dimensionCount, tableCount, features:[...]}`; `?cells=1` adds table cell text |
| `/mfg` | | | x | | `{ok, fileName, mfg_type, partial, partial_missing[], modelCount, featureCount, models[], features[]}` |
| `/harness-probe` | | x | | | diagnostic dump of what Toolkit knows about harnesses (see [Harness](#cabling-harnesses)) |
| `/open?fileName=&path=` | x | x | x | x | Opens a model from disk **in a Creo window** (see [Open](#open-a-model-in-creo)): `{ok, fileName, type, window, alreadyOpen, loadedFromDisk}` |
| `/url?url=` | | | | | Loads an address in Creo's **main embedded browser** (see [Load a URL](#load-a-url-in-the-embedded-browser)): `{ok, url, window, widened}` |
| `/collect?domains=a,b` | x | x | x | x | `{ok, fileName, type, domains:{a:{...}, b:{...}}}` |

**Mapping to CreoPDM's metadata fields:** take each endpoint's inner key: `parameters`, `mass`, `units`, `materials`, `identity`, `features`; `/structure` `.components` -> `structure`; `/bom` `.bom` -> `bom`; `relations`, `constraints` and `simp_reps` are the whole response. A parameter `value` is always a string (`"true"/"false"` for BOOLEAN); `units` is set only for DOUBLE.

### Collect contract
`GET|POST /collect?domains=a,b,c[&fileName=<name.ext[.N]>&path=<absolute path>][&keep=1][&recurse=0|1][&cells=1]`

- **Which domains:** a comma list (`identity,parameters,features,materials,mass,units,...`). Omitted or `all` runs every domain that applies to the model type and leaves out the rest. `bom` and `harness_probe` are **never** in the default set; ask for them explicitly. With an explicit list, a domain that does not apply returns its own `{ok:false, error}` while the others still run.
- **Which model:** without `fileName` the active model is used. With `fileName` (+ `path`, URL-encoded, the file or its folder):
  - already in session -> used as is (nothing opened, nothing erased);
  - not in session -> retrieved **once, silently** with `ProMdlFiletypeLoad`: loaded into memory but **not displayed, no window or tab is created, and it does not become the current model**;
  - not in session and no `path` -> `{ok:false, "... is not in session; pass path= ..."}` (never a windowed open).
- **One retrieve per request:** all requested domains run on that single in-session model. Separate HTTP calls for the same file are separate requests, so send one `/collect` with every domain you want.
- **Erase:** by default, a model **this call retrieved** is erased afterward with `ProMdlErase` (that one model only; never one that was already in session, never the current model; components pulled in with an assembly stay in session, undisplayed). **`keep=1`** skips the erase, so later calls for the same file reuse the in-session model with no reload. Cleanup of kept models is up to the caller.
- **Single-threaded and queued:** all Toolkit calls run on Creo's main thread, one request at a time (a hidden message-only window receives each request; Creo's own message pump runs it). Concurrent callers queue. If Creo is busy (modal dialog, regeneration) a request times out with `"Creo busy ... timed out"`; `/health` always answers within 3 s.
- **`timings_ms`** is appended to every `/collect` and single-domain response: `{retrieved, retrieve_ms, <domain>_ms for each domain that ran, erase_ms, total_ms}`, and the same line is logged, e.g.
  `/collect base-plate.prt retrieved=1 retrieve=842ms identity=1ms features=6ms mass=3ms erase=2ms total=861ms`.
- Example:
  ```powershell
  Invoke-RestMethod "http://127.0.0.1:8767/collect?domains=identity,features&fileName=base-plate.prt&path=C%3A%5Cmodels%5Cbase-plate.prt.4&keep=1"
  ```

### Load a URL in the embedded browser
`GET|POST /url?url=<absolute http(s) address, percent-encoded>[&percent=10..95]` loads the address in Creo's **main embedded browser** (`ProWindowURLShow` on the current window), not in the navigator pane. It is used to open a CreoPDM page (e.g. Details) from the explorer pane.
- Only `http://` and `https://` addresses are accepted. If `CREOPDM_URL` is set, the address must be on the same scheme, host and port; this stops other local web pages from steering Creo's browser elsewhere.
- The browser is widened to `percent` of the Creo window (default `CREOPDM_BROWSER_PERCENT`, else 50) only if it is currently narrower; it is never shrunk.
- Response: `{ok, url, window, widened}` or `{ok:false, error}`. Loopback only, like the other endpoints.

### Open a model in Creo
`GET|POST /open?fileName=<name.ext[.N]>&path=<absolute path to the file>` (either parameter alone is enough; a missing `fileName` is taken from the end of `path`). Unlike `/collect`, this **does** show the model:
- The file is loaded silently if it is not already in session (same checks as Collect: harness parts and missing files return the same `ok:false` messages).
- Then it is displayed the way File > Open does: in a **new window**, which is activated. If the model is already shown in a window, that window is activated instead, so repeated Opens do not add tabs.
- The model is **not** erased afterwards.
- Response: `{ok, fileName, type, window, alreadyOpen, loadedFromDisk}`, or `{ok:false, error}` (for example `could not display <file> in a window (Toolkit error N)`, which is what Creo returns if its open-window limit is reached).
- It is meant to be called by the CreoPDM agent (after it has downloaded the file to a local folder), not by a web page.

### Non-ASCII names (UTF-8 rules)
Everything the service sends is **UTF-8**: JSON bodies (`Content-Type: application/json; charset=utf-8` on every response), log lines and URLs. Model names, parameter values, feature names and paths with characters such as `é`, `ü` or CJK come out as proper UTF-8.
- Creo's UTF-16 strings are converted only with `WideCharToMultiByte(CP_UTF8)` (`w2u`, no default/substitute character). Character tests on those bytes are **ASCII-only** (`ascii_alnum`, `ascii_xdigit`, `upper_ascii`, ...); the C library's `tolower` / `isalnum` / `isxdigit` are locale-dependent and, in a Windows-1252 locale (which a host process like Creo can set), rewrite UTF-8 bytes such as `0xC3` into invalid sequences. That is what produced invalid JSON for a model called `café.prt` (`toolkit_bad_json` in the agent).
- **Model names are lower-cased as Unicode** (`lower_utf8`: Windows invariant-locale mapping, not the process locale). Creo stores names upper-case (the model above is `CAFÉ`, with `É` = U+00C9, bytes `C3 89`), while the file on disk is `café.prt` (`é` = U+00E9, bytes `C3 A9`). Lower-casing only ASCII letters gave `cafÉ.prt`; the full mapping gives `café.prt` and leaves names without case (e.g. `部品.prt`) unchanged.
- The JSON writer escapes quotes, backslashes (Windows paths) and control characters, writes valid non-ASCII characters as raw UTF-8, and writes any invalid byte as `\ufffd`, so a client can always decode a response as UTF-8.
- Request values (`fileName=`, `path=`, `url=`) are percent-decoded to bytes and must be valid UTF-8 (`café.prt` -> `caf%C3%A9.prt`). Invalid UTF-8 is rejected with a clear error instead of being guessed. `CREOPDM_URL` and `CREOPDM_EXPLORER_URL` are read as wide environment strings. The log file is opened with a wide path, so a TEMP folder under a non-ASCII user name works.
- `w2u` into a too-small buffer truncates on a character boundary instead of returning an empty string.

**Automated check (no Creo needed):** `tests\run_utf8_test.bat` builds `tests\utf8_test.c` against `src\textutil.c`, switches the C locale to Windows-1252 and verifies the points above (it also shows the old `tolower` corrupting UTF-8). It needs only Visual Studio's C compiler.

**Manual check in Creo** (model named `café.prt` active):
```powershell
$r = Invoke-WebRequest http://127.0.0.1:8767/health
$r.Headers['Content-Type']                       # application/json; charset=utf-8
($r.Content | ConvertFrom-Json).model.fileName   # café.prt  (a proper é)
# the same check the agent does:
curl.exe -s http://127.0.0.1:8767/health | python -c "import sys,json; json.loads(sys.stdin.buffer.read().decode('utf-8')); print('valid UTF-8 JSON')"
# round trip a non-ASCII name, parameter value and path through /collect (open or retrieve café.prt first):
$u = "http://127.0.0.1:8767/collect?domains=identity,parameters&fileName=caf%C3%A9.prt&path=" + [uri]::EscapeDataString("C:\models\café\café.prt")
(Invoke-WebRequest $u).Content | ConvertFrom-Json
```
Expected: `identity.file_name` is `café.prt`, any parameter value containing `é` shows it intact, and nothing decodes as `?` or mojibake.

### Quick test matrix
With Creo running and an appropriate model active:
```powershell
$b = "http://127.0.0.1:8767"
Invoke-RestMethod "$b/health"
(Invoke-RestMethod "$b/collect").domains.PSObject.Properties.Name
Invoke-RestMethod "$b/structure" | ConvertTo-Json -Depth 6
```
| Active model | Try |
|---|---|
| Part | `/identity` `/units` `/parameters` `/mass` `/materials` `/features` `/relations`; `/structure` must answer "applies to ASSEMBLY" |
| Assembly | `/structure` `/bom` `/mass` `/features` `/constraints` `/simp-reps` `/relations`; `/collect` |
| Drawing | `/identity` `/drawing` `/parameters`; `/mass` must answer "applies to PART/ASSEMBLY" |
| MFG | `/identity` `/parameters` `/units` `/mfg`; `/structure` must answer "(use /mfg)" |

A sample of an early `/structure` response is in [samples/tdd-demo.structure.json](samples/tdd-demo.structure.json) (current rows also carry `subtype`, `is_skeleton`, `is_harness` and `path`).

---

## Domains in detail

### Identity
- `file_name` is always the logical tip name (`shaft.prt`, lowercase) from the model handle; if the handle gives no name it falls back to the requested `fileName=` as given (e.g. `shaft.prt.2`).
- `model_role` is `SKELETON`, `SHEETMETAL` or `HARNESS` when known, plus a `subtype` label; `generic_name` and `instance_name` are empty strings (not derived) and `version_stamp` is null.
- A model that cannot be retrieved gives `ok:false "could not retrieve <file> ..."` with no identity at all; an unusable handle gives `ok:false "model handle for <file> is not usable"`; `/identity` never returns `ok:true` with an empty identity.
- **MFG:** `common_name` is the MFG file name and `origin` the folder of Toolkit's origin plus the MFG file name (`origin_derived:true`; the `.N` version is not available). Toolkit's own values, which describe the related assembly, are kept in `identity.related_assembly {common_name, origin}`.

### Mass
`mass`, `volume`, `surface_area`, `density`, `gravity_center`, `principal_moments`; `ok:true` only when real values exist. With no material or density the response is `ok:false` (plus `volume` if known); it never returns zeros.
For assemblies it tries `ProSolidMassPropertyGet`, `ProAssemblySolidMassPropertyGet`, then `ProSolidMassPropertyWithDensityGet`; if all fail it sums the leaf parts **already in session** (mass, volume, area, mass-weighted centre of gravity; `method:"component_sum"`). A sum is returned only when every non-suppressed part resolved, otherwise
`ok:false "assembly mass unavailable (<each return code>; component sum: N with mass, M without density, K unresolved)"`. The component sum has no inertia: `principal_moments` and `cg_inertia_tensor` are omitted. `method` says which path produced the numbers.

### Features
- Component features are excluded (they belong to `/structure`). **Internal (invisible) features** (`ProFeatureVisibilityGet` = FALSE) and any feature whose raw name is Creo's `no_name` placeholder are skipped everywhere features are listed (`/features`, `/relations` feature rows, `/mfg`); the literal `no_name` is never emitted. Unnamed but visible features appear as `TYPE (id)`.
- Type names are normalised (`DATUM_PLANE`).
- **Groups nest their members.** Group membership comes from `ProFeatureGroupGet`. `level` is 1 for top-level features; a `GROUP_HEAD` stays at its own level and is followed by its members at `level+1` (nested groups go deeper). A member whose group head is not in the list (e.g. hidden) appears at level 1, so nothing is lost. Applies to ordinary and cabling groups.
  ```json
  [{"id":210,"name":"BOLTS","type":"GROUP_HEAD","subtype":"","level":1,"status":"ACTIVE","suppressed":false},
   {"id":214,"name":"BOLT_1","type":"COMPONENT","subtype":"","level":2,"status":"ACTIVE","suppressed":false}]
  ```

### Structure
Full nested tree by default (`?recurse=0` for the top level only). Row: `{id, name, featureName, type, subtype, is_skeleton, is_harness, level, status, suppressed, path, expanded}`.
- `name` is the model tip with extension (`shaft.prt`, `sub.asm`; lowercase, no `.N`). `type` is PART, ASSEMBLY, SKELETON or HARNESS, never COMPONENT. Only real components are walked, so datums, CSYS and cross-sections cannot appear (they are in `/features`).
- Levels: top-level components 1, children of a sub-assembly 2, and so on. `path` is the component-id chain from the root (`"46"`, `"40/669"`).
- Only component features are walked (one `ProSolidFeatVisit` per assembly); nothing of a child's features, parameters, materials or mass is read, and **nothing is retrieved**. A sub-assembly is expanded only if its model is already in memory (always true after the root assembly was loaded, since Creo loads its components). A non-suppressed sub-assembly not in memory is listed by name with `expanded:false`, and the response has `partial:true` and `unexpandedSubassemblies:N`. Suppressed sub-assemblies are never expanded.
- **Skeletons:** `type:"SKELETON"`, `subtype:"SKELETON"`, `is_skeleton:true` when `ProMdlIsSkeleton` says so (model in memory) or, for a name-only component, when it is the owner assembly's primary skeleton. `is_skeleton:null` means unknown (model not in memory), and `type` is then a best guess of PART.

### BOM
`GET /bom` or `domains=bom` (assemblies; explicit only).
- Rolled up per assembly level by **logical tip name**; `quantity` = non-suppressed occurrences under ONE instance of the parent (not multiplied by the parent's quantity). Suppressed components are excluded (`suppressedExcluded`).
- Each unique sub-assembly is expanded once if its model is in session; nothing is retrieved. Depth limit 6. A sub-assembly that cannot be expanded is a leaf with `expanded:false` and `leaf_reason:"not_in_memory"` (or `"max_depth"`), and the response has `partial:true`.
- Node: `{filename, quantity, dependency_type: ASSEMBLY_ROOT|ASSEMBLY_MEMBER, type: PART|ASSEMBLY|SKELETON|HARNESS, children[], expanded?, leaf_reason?}`. `bom` is a one-element array holding the `ASSEMBLY_ROOT` wrapper. Harness members are listed with `type:"HARNESS"` (never loaded); skeletons with `type:"SKELETON"` so a consumer can filter them.
  ```json
  {"ok":true,"fileName":"main.asm","partial":false,"bom":[{"filename":"main.asm","quantity":1,"dependency_type":"ASSEMBLY_ROOT","type":"ASSEMBLY","expanded":true,"children":[
     {"filename":"base-plate.prt","quantity":1,"dependency_type":"ASSEMBLY_MEMBER","type":"PART","children":[]},
     {"filename":"bracket.asm","quantity":2,"dependency_type":"ASSEMBLY_MEMBER","type":"ASSEMBLY","expanded":true,"children":[
        {"filename":"arm.prt","quantity":1,"dependency_type":"ASSEMBLY_MEMBER","type":"PART","children":[]}]}]}]}
  ```
  (Shortened, hand-written example of the shape.)

### Constraints and simplified representations
- **`/constraints`:** per-component constraints with the full constraint-type decode (MATE, ALIGN, INSERT, DEFAULT, FIX, AUTO, angle and distance types, ...) and the raw `type_id`; `offset_units` is the owner assembly's length unit (`deg` for angular types); `packaged` per component. A DEFAULT or FIX placement has `null` references by design. The reference objects (`item_type, id, name, path`) and the component wrapper are Toolkit-native; datum sides are raw enum values. A harness with no component feature is skipped.
- **`/simp-reps`:** `id` / `name` / `is_active`. Toolkit's non-positive sentinel ids (e.g. DEFAULT REP) are reported as `null`; the active rep is matched by name; `active.is_master` is true when the master representation is active.

### Drawings
`/drawing` (and `/features` on a drawing) returns an outline-ordered inventory: `SHEET 1`, every view / note / dimension / table on sheet 1, then `SHEET 2` and its children, and so on.
- Row: `id` (unique string: `sheet:N`, `view:N`, `note:N`, `dim:<owner>:N`, `table:N`), `creo_id`, `name`, `type` (SHEET | VIEW | NOTE | DIMENSION | TABLE), `subtype`, `level`, `status`, `suppressed`, `sheet`, `sheet_id`, `parent_id`.
- Levels: sheets 1; views, notes, dimensions and tables 2 (a dimension's view is in `view_id` / `parent_id`). A row with no sheet (e.g. a drawing-owned dimension not tied to a view) goes last at level 1 with `sheet:null`.
- Extras: NOTE `text`; DIMENSION `value`, `value_displayed`, `units`, `units_basis`, `units_model`, `symbol`, `owner`, `view_id`, `reference`; TABLE `rows`, `columns`, `segments` (+ `cells` with `?cells=1`). A VIEW's `subtype` is the model it shows.
- `partial:true` with `partial_missing` only if notes, dimensions or tables could not be enumerated. Notes inside table cells and model annotations (3D notes, GD&T) are not listed.
- `/collect` on a drawing returns `identity`, `parameters` and `drawing`; `features` is left out of the default set (it would duplicate `drawing`); an explicit `domains=features` still returns the same inventory.

**Dimension units.** Values are never converted; `units` names the unit the value is stored in, and lengths always use a **model** length unit, never the drawing sheet unit:

| Dimension | `units` | `units_basis` | `units_model` |
|---|---|---|---|
| ANGLE | `deg` | `angle` | null |
| Shown from a model | the owner model's length unit (e.g. `mm`) | `owner_model` | the owner model |
| Created in the drawing | the length unit of the model it measures: its attachment's model, else its view's model, else the first model in the drawing | `owner_model` | that model |

`units_basis:"unresolved"` (units null) means no model was found. `value` is as Toolkit returns it; `value_displayed` is the rounded value as shown. `subtype` is LINEAR / RADIUS / DIAMETER / ANGLE / ARC_LENGTH with the raw `type_id`; another value gives `OTHER`, a failed call `UNKNOWN`, both with `units_assumed:true`. Ordinate dimensions are not detected separately.

### MFG
`/mfg` reads session data only (nothing is retrieved). **No solid feature walk is run on an MFG handle** (it is not a solid): `/structure`, `/constraints`, `/features`, `/mass`, `/materials`, `/relations` and `/simp-reps` answer with a clear message (`... (use /mfg)` where relevant).
- `models[]`: components of the MFG top-level assembly, ids `mfgmdl:N`, with `role` REF_MODEL | WORKPIECE | FIXTURE, type, status, suppressed.
- `features[]`: features of the MFG storage solid: operations `mfgop:N`, workcells `mfgwc:N`, setups/fixtures `mfgfix:N`, stock models `mfgstock:N` (`type:"STOCK_MODEL"`), NC sequences `mfgseq:N`, other `mfgfeat:N`. `type` is that kind, `subtype` the Toolkit feature type name, `creo_id` the numeric id. COMPONENT features (they are in `models[]`), datum / csys / curve features and internal features are omitted. NC sequences are recognised by feature-type-name keywords (Toolkit has no single NC-sequence type), so an unusual one can show as `OTHER`; its `subtype` says what it is.
- **Nesting:** operations, workcells, setups/fixtures and stock are `level:1`. Every other feature (sequences, mill windows/volumes/surfaces, drill groups, ...) is `level:2` with `parent_id:"mfgop:N"` when its owner can be determined from Toolkit links, never guessed from visit order:
  1. `parent_basis:"operation_reference"`: an operation is among the feature's direct parents (`ProFeatureParentsGet`);
  2. `parent_basis:"referenced_by_sequence"`: no direct operation, but the features that depend on it (`ProFeatureChildrenGet`, up to 4 levels) all belong to the same operation.

  A feature used by several operations, or by none, stays unowned: `level`, `parent_id` and `parent_basis` are null and `"nesting"` is added to `partial_missing` (`partial:true`).
- `/collect` on an MFG returns `identity`, `units`, `parameters`, `mfg`. Tools, sequence parameters and cl-data are not read.

### Cabling harnesses
A harness model cannot be opened on its own in Creo ("open the cabling assembly which contains this harness"), so the service never tries.
- **Detection without loading:** Toolkit has no API to inspect a model without loading it, so the file's first line (<= 200 bytes) is read before any load. Creo-saved harness parts start with `#UGC:2 PART/HARNESS_PART ...` (sheetmetal `PART/SHEETMETAL`, normal parts `PART`). The check runs only for a model not already in session; a harness opened through its cabling assembly is in session and collects normally. The same check is repeated after a failed load. Verified against Creo's own `striped_cable/_striped_cable_preview_harness.prt`; a harness that is not recognised falls through to the generic load error.
- **Identity succeeds, everything else is skipped.** For a harness tip not in session, `identity` is answered from the file header alone:
  ```json
  GET /identity?fileName=600640041454.prt&path=C%3A%5C...%5C600640041454.prt.3
  {"ok":true,"fileName":"600640041454.prt",
   "identity":{"file_name":"600640041454.prt","model_type":"PART","model_role":"HARNESS","subtype":"HARNESS",
               "origin":"C:\\...\\600640041454.prt.3","identity_source":"file_header"}}
  ```
  `/collect` for such a tip is `ok:true` with `"harness":true`, `"type":"PART"`, and a per-domain result: `identity` succeeds; `parameters`, `features`, `materials`, `mass`, `units`, `relations` and every other domain return `ok:false` with the `harness_not_openable_in_isolation` message, so a client can save the identity and skip the rest.
- **Harness members in `/structure` and `/bom`.** A component is a harness when it is in its owner assembly's Toolkit harness list (`ProAssemblyHarnessesTopCollect`) or, for an in-memory model, its on-disk header says `HARNESS`. A supplement pass per assembly adds any harness from that list the component visit did not report (`id:null` if no component feature references it); with recursion on, a root-level pass over `ProAssemblyHarnessesCollect` (the assembly and its sub-assemblies) adds any still missing, at level 1. Rows have `type:"HARNESS"`, `subtype:"HARNESS"`, `is_harness:true`; nothing is loaded.
  ```json
  {"id":612,"name":"600640041454.prt","featureName":null,"type":"HARNESS","subtype":"HARNESS",
   "is_skeleton":false,"is_harness":true,"level":1,"status":"ACTIVE","suppressed":false,"path":"612","expanded":false}
  ```
- **`/harness-probe`** (assembly only, diagnostic) dumps what Toolkit knows: the return codes and model names of both harness lists, spools, connector count, every `GROUP_HEAD` with its members, and all cabling-type features. If a harness is in neither list and not a group member, Toolkit does not expose it as a model for that assembly and `/structure` and `/bom` cannot list it.

---

## Expected skips / error strings
`/collect` and the domain endpoints return `{ok:false, error:"..."}` with these messages when a model cannot be targeted. They are safe to treat as expected skips:

| Error prefix | Meaning |
|---|---|
| `harness_not_openable_in_isolation: open the cabling assembly that contains <tip>` | The file is a cabling harness part; no load is attempted. |
| `<tip> is not in session; pass path= ...` | Not in session and no `path` was given. |
| `file_not_found: <path> (ProMdlFiletypeLoad=N)` | The path does not exist. |
| `could not retrieve <tip> from path (ProMdlFiletypeLoad=N)` | Any other load failure (for example a file saved in a newer Creo than the one running). |
| `<domain> applies to PART/ASSEMBLY/...; active model is X` | The domain does not apply to that model type. |

---

## Developer notes
| File | Role |
|---|---|
| `src/main.c` | `user_initialize` / `user_terminate`, the CreoPDM ribbon commands (CreoPDM and CreoPDM Browse) |
| `src/http_server.c` | the 127.0.0.1 server, worker thread, main-thread marshalling |
| `src/router.c` | path -> collector table, `/health`, `/model`, `/collect`, `/open`, targeting and silent retrieve, timings |
| `src/textutil.c/.h` | string builder, JSON writer, UTF-8/UTF-16 conversion, ASCII helpers, query parsing (no Toolkit dependency; unit-tested) |
| `src/common.c/.h` | logging, error envelopes, model context |
| `src/structure.c` | component walker (shared by structure, BOM, constraints, mass) and `/structure` |
| `src/model.c` | identity, units, parameters, mass, materials |
| `src/features.c` | features (with group nesting) and relations |
| `src/dimensions.c` | model dimensions per feature |
| `src/sketch.c` | 2D section (`.sec`) contents |
| `src/assembly.c` | constraints, simplified representations |
| `src/bom.c` | nested BOM |
| `src/drawing.c` | drawing inventory |
| `src/mfg.c` | MFG inventory |
| `src/cabling.c` | harness probe |

**Threading.** Toolkit is not thread-safe, so the HTTP worker thread never calls it. `user_initialize` runs on Creo's main thread and creates a hidden message-only window; each request is posted to it and executed in its window procedure through Creo's own message pump. Requests are handled one at a time, so work never overlaps. If the port is already in use, the server is skipped, the reason is logged, and the message line says so; the ribbon commands still work.

**Add a domain.** Write `collect_x(Sb*, const Ctx*, const Query*)` and add one row to `DOMAINS[]` in `router.c`. It then gets its own `GET /x` and is available through `/collect?domains=x`.
