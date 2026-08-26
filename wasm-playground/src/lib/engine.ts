import type * as duckdb from '@duckdb/duckdb-wasm/dist/duckdb-browser';

import { DEMO_SEED_SQL } from '../demo/scenarios';
import { runtimeBundle } from './runtime';

export type CellValue = string | number | boolean | null;

export interface QueryOutput {
  columns: string[];
  rows: CellValue[][];
  elapsedMs: number;
  error?: string;
}

interface ArrowTableLike {
  schema: { fields: Array<{ name: string }> };
  toArray(): Array<Record<string, unknown>>;
}

function normalizeCell(value: unknown): CellValue {
  if (value === null || value === undefined) {
    return null;
  }

  if (typeof value === 'string' || typeof value === 'number' || typeof value === 'boolean') {
    return value;
  }

  if (typeof value === 'bigint') {
    return value.toString();
  }

  if (value instanceof Date) {
    return value.toISOString();
  }

  return JSON.stringify(value);
}

export function tableToQueryOutput(table: ArrowTableLike, elapsedMs: number): QueryOutput {
  const columns = table.schema.fields.map((field) => field.name);
  const rows = table.toArray().map((row) => columns.map((column) => normalizeCell(row[column])));

  return { columns, rows, elapsedMs };
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

export class PlaygroundEngine {
  private db: duckdb.AsyncDuckDB | null = null;
  private connection: duckdb.AsyncDuckDBConnection | null = null;
  private selectedVariant: 'mvp' | 'eh' | null = null;

  get variant(): 'mvp' | 'eh' | null {
    return this.selectedVariant;
  }

  async initialize(): Promise<void> {
    if (this.connection) {
      return;
    }

    // Keep the browser-only worker bundle out of SSR/unit-test module loading.
    // The runtime is instantiated only from the browser action that needs it.
    const duckdb = await import('@duckdb/duckdb-wasm/dist/duckdb-browser');
    const paths = runtimeBundle(import.meta.env.BASE_URL);
    // Pin the MVP core: selectBundle prefers the EH variant, and the EH pair
    // currently fails to instantiate inside a dedicated worker while the MVP
    // pair boots cleanly (verified via an isolated-worker probe).
    void paths.eh;
    const selected = { ...paths.mvp };
    const worker = new Worker(selected.mainWorker ?? paths.mvp.mainWorker);
    // Surface otherwise-silent worker failures during instantiation.
    const onWorkerFailure = (event: ErrorEvent | MessageEvent): never => {
      const detail =
        'message' in event && typeof event.message === 'string'
          ? event.message
          : String((event as MessageEvent).data ?? 'unknown');
      throw new Error('[worker] ' + detail);
    };
    const db = new duckdb.AsyncDuckDB(new duckdb.ConsoleLogger(), worker);

    try {
      // The harness's instantiate promise can stall silently if the worker dies;
      // surface worker failures through the same error channel.
      await Promise.race([
        db.instantiate(selected.mainModule, null),
        new Promise<never>((_, reject) => {
          worker.onerror = (event: ErrorEvent) => {
            try {
              onWorkerFailure(event);
            } catch (error) {
              reject(error);
            }
          };
          worker.onmessageerror = (event: MessageEvent) => {
            try {
              onWorkerFailure(event);
            } catch (error) {
              reject(error);
            }
          };
        }),
      ]);
      const connection = await db.connect();
      await connection.query(DEMO_SEED_SQL);
      this.db = db;
      this.connection = connection;
      this.selectedVariant = 'mvp';
    } catch (error) {
      await db.terminate();
      throw error;
    }
  }

  async run(sql: string): Promise<QueryOutput> {
    if (!this.connection) {
      throw new Error('DuckDB-Wasm is still starting. Please try again in a moment.');
    }

    const startedAt = performance.now();
    try {
      const table = await this.connection.query(sql);
      return tableToQueryOutput(table, Math.round(performance.now() - startedAt));
    } catch (error) {
      return {
        columns: [],
        rows: [],
        elapsedMs: Math.round(performance.now() - startedAt),
        error: errorMessage(error),
      };
    }
  }

  async reset(): Promise<void> {
    await this.close();
    await this.initialize();
  }

  async close(): Promise<void> {
    if (this.connection) {
      await this.connection.close();
    }
    this.connection = null;
    this.selectedVariant = null;

    if (this.db) {
      await this.db.terminate();
    }
    this.db = null;
  }
}
