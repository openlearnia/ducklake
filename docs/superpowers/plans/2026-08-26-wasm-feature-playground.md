# Custom DuckDB-Wasm Feature Playground Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Build and publish a React playground backed by custom DuckDB-Wasm modules that demonstrates the fork's DuckLake materialized views, refresh history, snapshots, and JavaScript procedures.

**Architecture:** Use the upstream DuckDB-Wasm TypeScript/browser-worker package with a custom core compiled against the nested DuckDB fork. Statically include the DuckLake extension in that core, select EH or MVP at runtime, and ship the modules as immutable static assets with a Vite app deployed to Cloudflare Pages.

**Tech Stack:** C++ DuckDB/DuckLake fork, Emscripten, DuckDB-Wasm TypeScript API, React, Vite, TypeScript, Vitest, Playwright, Cloudflare Pages.

**Spec:** `docs/superpowers/specs/2026-08-26-wasm-feature-playground-design.md`

## Global Constraints

- Browser demo uses only local seeded data; no production credentials or remote Postgres catalog.
- Custom DuckDB-Wasm core must be built from the nested fork checkout, not the stock npm WASM binary.
- Generate MVP and EH modules; do not commit generated binaries.
- SQL errors must remain visible; never replace failed runtime queries with mocked result rows.
- Native-only Postgres scanner, Quack server calls, and native HTTPFS are outside the browser feature set.

### Task 1: Define tested playground contracts

**Files:**
- Create: `wasm-playground/src/demo/scenarios.test.ts`
- Create: `wasm-playground/src/lib/runtime.test.ts`

**Interfaces:**
- `Scenario` exposes `id`, `title`, `summary`, `sql`, and `focus`.
- `runtimeBundle(baseUrl)` returns MVP and EH module/worker URLs.

- [ ] **Step 1: Write failing tests** for scenario order/content and runtime URL generation.
- [ ] **Step 2: Run the focused tests and confirm they fail because the modules do not exist.**
- [ ] **Step 3: Implement the smallest scenario and runtime contracts.**
- [ ] **Step 4: Run the focused tests and confirm they pass.**

### Task 2: Build the React playground shell

**Files:**
- Create: `wasm-playground/package.json`, `tsconfig.json`, `vite.config.ts`, `index.html`
- Create: `wasm-playground/src/main.tsx`, `src/App.tsx`, `src/styles/app.css`
- Create: `wasm-playground/src/lib/PlaygroundEngine.ts`
- Create: `wasm-playground/src/components/ScenarioRail.tsx`, `src/components/SqlWorkspace.tsx`, `src/components/ResultTable.tsx`, `src/components/StatusPanel.tsx`
- Test: `wasm-playground/src/demo/scenarios.test.ts`, `wasm-playground/src/lib/runtime.test.ts`

**Interfaces:**
- `PlaygroundEngine.initialize(): Promise<void>` seeds the local connection.
- `PlaygroundEngine.run(sql: string): Promise<QueryOutput>` executes SQL and returns columns, rows, elapsed milliseconds, and error text.
- `PlaygroundEngine.reset(): Promise<void>` recreates the demo state.

- [ ] **Step 1: Add the package and test scripts.**
- [ ] **Step 2: Add the loading/error/empty UI states and SQL workspace.**
- [ ] **Step 3: Implement the worker-backed engine using `AsyncDuckDB`.**
- [ ] **Step 4: Add feature scenario selection and reset/run behavior.**
- [ ] **Step 5: Run typecheck, lint, focused tests, and Vite build.**

### Task 3: Add the custom WASM core build

**Files:**
- Create: `wasm-build/scripts/build-core.sh`
- Create: `wasm-build/extension_config_wasm.cmake`
- Create: `wasm-build/duckdb-wasm-custom.patch`
- Modify: `.github/workflows/GrainExtensionPublish.yml`

**Interfaces:**
- `build-core.sh <mvp|eh> <duckdb-wasm-dir> <ducklake-dir> <output-dir>` produces the selected `duckdb-*.wasm` and generated JS wrapper.
- The CI job produces `duckdb-mvp.wasm` and `duckdb-eh.wasm` in the app runtime staging directory.

- [ ] **Step 1: Add a CI-only build script that points DuckDB-Wasm at the nested fork.**
- [ ] **Step 2: Add the external-project argument that passes the DuckLake extension config into the nested DuckDB build.**
- [ ] **Step 3: Add the WASM build job with Emscripten and matching DuckDB-Wasm worker/API assets.**
- [ ] **Step 4: Run the build script in CI and fail if the custom module is missing.**

### Task 4: Add static deployment and browser smoke coverage

**Files:**
- Create: `wasm-playground/public/_headers`
- Create: `wasm-playground/playwright.config.ts`
- Create: `wasm-playground/tests/playground.spec.ts`
- Create: `.github/workflows/wasm-playground.yml`
- Modify: `wasm-playground/package.json`

**Interfaces:**
- CI accepts a successful custom WASM artifact only when the React build and browser smoke pass.
- Cloudflare Pages receives the final `wasm-playground/dist` directory.

- [ ] **Step 1: Add production asset headers and Playwright smoke test.**
- [ ] **Step 2: Add CI artifact staging and app build.**
- [ ] **Step 3: Add Cloudflare Pages deployment on the configured release branch/manual dispatch.**
- [ ] **Step 4: Run local static checks and trigger CI.**

### Task 5: Verify the published result

- [ ] **Step 1: Verify the CI workflow completed with both WASM variants.**
- [ ] **Step 2: Verify the deployed URL serves the app, worker, and WASM module.**
- [ ] **Step 3: Run the browser smoke against the deployed URL.**
- [ ] **Step 4: Record any browser limitations in the playground README.**
