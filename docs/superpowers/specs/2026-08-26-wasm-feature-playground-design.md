# Custom DuckDB-Wasm Feature Playground

**Status:** Approved for implementation

## Goal

Publish a browser-only React playground that executes the OpenLearnia DuckDB
preview fork in a Web Worker and makes the custom DuckLake materialized-view,
refresh-history, snapshot, and JavaScript-procedure features interactive.

## Scope

The first release is a self-contained demonstration. It seeds a small local
DuckLake catalog, runs SQL in the browser, and displays query results plus a
feature-specific explanation. It does not connect to a production Postgres
catalog and does not attempt to expose native-only `postgres_scanner`, Quack,
or native HTTPFS behavior in the browser.

## Architecture

The custom core is built from the DuckDB-Wasm wrapper sources with the nested
OpenLearnia DuckDB checkout supplied as `DUCKDB_LOCATION`. A small external
extension configuration statically includes the sibling DuckLake source. The
build produces `duckdb-mvp.wasm` and `duckdb-eh.wasm`; the browser selects EH
when available and falls back to MVP. The TypeScript API and browser workers
come from the pinned DuckDB-Wasm package and load those custom modules.

The React app lives in `wasm-playground/`. A single `PlaygroundEngine` owns
the asynchronous DuckDB connection, applies the seed script, executes the
selected scenario, and returns column names, rows, elapsed time, and engine
metadata. The UI has a scenario rail, SQL editor, run/reset controls, a result
grid, and an engine/feature status panel. Scenario definitions are plain data
so they can be tested without a browser or WASM runtime.

## Scenarios

The initial scenarios are:

1. Materialized view: create a managed view, query it, refresh it, and show the
   resulting rows.
2. Refresh history: inspect `ducklake_materialized_view_refresh_history` after
   a refresh and display the recorded mode and snapshot values.
3. Snapshots: query the current and prior DuckLake snapshots and compare the
   visible result.
4. JavaScript procedure: create a typed JavaScript procedure and call it.

Each scenario is executable from the SQL editor, with a reset action that
recreates the in-memory demo state.

## Build and publish contract

CI provisions Emscripten and builds both WASM variants from a clean checkout.
The generated core modules and matching worker/API assets are staged under
`wasm-playground/public/runtime/` during the web build; generated binaries are
never committed. The app build fails if the custom runtime modules are absent.

The app is a static Vite bundle deployed to Cloudflare Pages. The workflow
builds the custom runtime first, copies it into the app artifact, runs the app
build and browser smoke test, then deploys the final `dist/` directory.

## Security and failure behavior

The app never accepts database credentials or arbitrary remote catalog URLs.
The SQL editor is limited to the local demo connection. Runtime load failures
show the exact failed stage and preserve the editor contents. SQL errors are
shown as errors in the result pane rather than silently falling back to a
mocked result.

## Verification

- Pure scenario and runtime-contract tests pass.
- TypeScript, lint, and Vite production build pass.
- A browser smoke test proves the custom WASM module loads and executes the
  MV and JavaScript-procedure scenarios.
- CI artifacts contain both custom WASM variants and the deployed static app.
