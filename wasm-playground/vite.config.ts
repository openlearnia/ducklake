import { fileURLToPath, URL } from 'node:url';
import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig({
  plugins: [react()],
  resolve: {
    alias: {
      // Pair the app with the DuckDB-Wasm JS API built from the same harness
      // source as our custom worker bundles - npm versions drift on the
      // worker message protocol.
      '@duckdb/duckdb-wasm/dist/duckdb-browser': fileURLToPath(
        new URL('./src/lib/vendor/duckdb-browser.mjs', import.meta.url),
      ),
    },
  },
  build: {
    target: 'es2022',
  },
});
