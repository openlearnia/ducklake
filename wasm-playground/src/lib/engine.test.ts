import { describe, expect, it } from 'vitest';
import { tableFromArrays } from 'apache-arrow';

import { tableToQueryOutput } from './engine';

describe('tableToQueryOutput', () => {
  it('projects Arrow rows into stable browser table data', () => {
    const table = tableFromArrays({
      region: ['North', 'South'],
      orders: [2, 1],
      revenue: [270, null],
    });

    expect(tableToQueryOutput(table, 12)).toEqual({
      columns: ['region', 'orders', 'revenue'],
      rows: [
        ['North', 2, 270],
        ['South', 1, null],
      ],
      elapsedMs: 12,
    });
  });

  it('keeps complex values renderable without leaking Arrow objects', () => {
    const table = tableFromArrays({
      metadata: [{ kind: 'refresh', snapshot: 4 }],
    });

    expect(tableToQueryOutput(table, 0).rows).toEqual([
      ['{"kind":"refresh","snapshot":4}'],
    ]);
  });
});
