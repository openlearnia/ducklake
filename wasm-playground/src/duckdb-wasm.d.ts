declare module '@duckdb/duckdb-wasm/dist/duckdb-browser' {
  export interface DuckDBBundle {
    mainModule: string;
    mainWorker: string | null;
    pthreadWorker: string | null;
  }

  export interface DuckDBBundles {
    asyncDefault: {
      mainModule: string;
      mainWorker: string;
    };
    asyncNext?: {
      mainModule: string;
      mainWorker: string;
    };
  }

  export class ConsoleLogger {
    log(entry: unknown): void;
  }

  export class AsyncDuckDBConnection {
    close(): Promise<void>;
    query(text: string): Promise<{
      schema: { fields: Array<{ name: string }> };
      toArray(): Array<Record<string, unknown>>;
    }>;
  }

  export class AsyncDuckDB {
    constructor(logger: ConsoleLogger, worker?: Worker | null);
    instantiate(mainModuleURL: string, pthreadWorkerURL?: string | null): Promise<null>;
    connect(): Promise<AsyncDuckDBConnection>;
    terminate(): Promise<void>;
  }

  export function selectBundle(bundles: DuckDBBundles): Promise<DuckDBBundle>;
}
