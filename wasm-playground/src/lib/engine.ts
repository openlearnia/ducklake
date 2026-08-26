import * as duckdb from '@duckdb/duckdb-wasm/dist/duckdb-browser';

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

    const paths = runtimeBundle(import.meta.env.BASE_URL);
    const bundles: duckdb.DuckDBBundles = {
      asyncDefault: paths.mvp,
      asyncNext: paths.eh,
    };
    const selected = await duckdb.selectBundle(bundles);
    const worker = new Worker(selected.mainWorker ?? paths.mvp.mainWorker);
    const db = new duckdb.AsyncDuckDB(new duckdb.ConsoleLogger(), worker);

    try {
      await db.instantiate(selected.mainModule, selected.pthreadWorker);
      const connection = await db.connect();
      await connection.query(DEMO_SEED_SQL);
      this.db = db;
      this.connection = connection;
      this.selectedVariant = selected.mainModule === paths.eh.mainModule ? 'eh' : 'mvp';
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
