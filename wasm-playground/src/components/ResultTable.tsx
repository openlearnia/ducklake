import type { QueryOutput } from '../lib/engine';

interface ResultTableProps {
  output: QueryOutput | null;
}

export function ResultTable({ output }: ResultTableProps) {
  if (!output) {
    return <div className="result-empty"><span className="empty-glyph">⌘</span><span>Choose a feature and run its SQL to see live results.</span></div>;
  }

  if (output.error) {
    return <div className="result-error"><strong>Query failed</strong><code>{output.error}</code></div>;
  }

  return (
    <section className="result-panel">
      <div className="result-toolbar">
        <div><div className="eyebrow">Result</div><strong>{output.rows.length} row{output.rows.length === 1 ? '' : 's'}</strong></div>
        <span className="elapsed">{output.elapsedMs} ms</span>
      </div>
      <div className="table-scroll">
        <table>
          <thead><tr>{output.columns.map((column) => <th key={column}>{column}</th>)}</tr></thead>
          <tbody>
            {output.rows.map((row, rowIndex) => (
              <tr key={rowIndex}>{row.map((cell, columnIndex) => <td key={`${rowIndex}-${columnIndex}`}>{cell === null ? <span className="null-value">NULL</span> : String(cell)}</td>)}</tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}
