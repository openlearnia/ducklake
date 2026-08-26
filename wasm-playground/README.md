# DuckLake WASM feature playground

This is a browser-only demonstration of the custom DuckDB/DuckLake build. It
seeds a small local DuckLake catalog in the worker and executes real SQL for:

- DuckLake-managed materialized views
- materialized-view refresh history
- snapshot metadata
- first-class JavaScript procedures

## Local app checks

The static UI can be checked without a WASM build:

```sh
npm ci
npm test
npm run typecheck
npm run build
```

The browser scenarios require the generated files in `public/runtime/`. CI
builds those files with Emscripten, copies the matching DuckDB-Wasm worker
bindings, and runs the Playwright smoke tests before publishing the Pages
artifact.
