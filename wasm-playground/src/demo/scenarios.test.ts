import { describe, expect, it } from 'vitest';
import { DEMO_SEED_SQL, scenarios } from './scenarios';

describe('playground scenarios', () => {
  it('keeps the feature tour in a stable user-facing order', () => {
    expect(scenarios.map((scenario) => scenario.id)).toEqual([
      'materialized-view',
      'refresh-history',
      'snapshots',
      'javascript-procedure',
    ]);
  });

  it('uses real custom-feature SQL in every scenario', () => {
    expect(DEMO_SEED_SQL).toContain("ATTACH 'ducklake:");
    expect(scenarios.find((scenario) => scenario.id === 'materialized-view')?.sql).toContain(
      'CREATE MATERIALIZED VIEW',
    );
    expect(scenarios.find((scenario) => scenario.id === 'refresh-history')?.sql).toContain(
      'ducklake_materialized_view_refresh_history',
    );
    expect(scenarios.find((scenario) => scenario.id === 'snapshots')?.sql).toContain(
      'ducklake_snapshots',
    );
    expect(scenarios.find((scenario) => scenario.id === 'javascript-procedure')?.sql).toContain(
      'LANGUAGE JAVASCRIPT',
    );
  });
});
