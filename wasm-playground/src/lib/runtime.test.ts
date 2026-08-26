import { describe, expect, it } from 'vitest';
import { runtimeBundle } from './runtime';

describe('runtimeBundle', () => {
  it('points both browser variants at same-origin custom assets', () => {
    expect(runtimeBundle('/playground/')).toEqual({
      mvp: {
        mainModule: '/playground/runtime/duckdb-mvp.wasm',
        mainWorker: '/playground/runtime/duckdb-browser-mvp.worker.js',
      },
      eh: {
        mainModule: '/playground/runtime/duckdb-eh.wasm',
        mainWorker: '/playground/runtime/duckdb-browser-eh.worker.js',
      },
    });
  });

  it('normalizes a base URL without creating double slashes', () => {
    expect(runtimeBundle('/')).toEqual({
      mvp: {
        mainModule: '/runtime/duckdb-mvp.wasm',
        mainWorker: '/runtime/duckdb-browser-mvp.worker.js',
      },
      eh: {
        mainModule: '/runtime/duckdb-eh.wasm',
        mainWorker: '/runtime/duckdb-browser-eh.worker.js',
      },
    });
  });
});
