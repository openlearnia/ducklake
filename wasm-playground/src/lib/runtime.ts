export interface RuntimeVariant {
  mainModule: string;
  mainWorker: string;
}

export interface RuntimeBundle {
  mvp: RuntimeVariant;
  eh: RuntimeVariant;
}

function joinUrl(baseUrl: string, path: string): string {
  const normalizedBase = baseUrl === '/' ? '' : baseUrl.replace(/\/+$/, '');
  return `${normalizedBase}/${path}`;
}

export function runtimeBundle(baseUrl: string): RuntimeBundle {
  return {
    mvp: {
      mainModule: joinUrl(baseUrl, 'runtime/duckdb-mvp.wasm'),
      mainWorker: joinUrl(baseUrl, 'runtime/duckdb-browser-mvp.worker.js'),
    },
    eh: {
      mainModule: joinUrl(baseUrl, 'runtime/duckdb-eh.wasm'),
      mainWorker: joinUrl(baseUrl, 'runtime/duckdb-browser-eh.worker.js'),
    },
  };
}
