interface StatusPanelProps {
  status: 'starting' | 'ready' | 'error';
  variant: 'mvp' | 'eh' | null;
  error: string | null;
}

const features = [
  'DuckLake managed tables',
  'Materialized views',
  'Refresh history',
  'JavaScript procedures',
];

export function StatusPanel({ status, variant, error }: StatusPanelProps) {
  const label = status === 'starting' ? 'Starting engine' : status === 'ready' ? 'Engine ready' : 'Engine unavailable';

  return (
    <section className="status-panel" aria-label="Runtime status">
      <div className="status-heading">
        <span className={`status-dot ${status}`} />
        <span>{label}</span>
        {variant && <span className="runtime-chip">{variant.toUpperCase()} / WASM</span>}
      </div>
      {error ? <p className="status-error">{error}</p> : <p>Custom DuckDB core, executing in a worker.</p>}
      <div className="feature-list">
        {features.map((feature) => (
          <span className="feature-pill" key={feature}>
            <span>✓</span>{feature}
          </span>
        ))}
      </div>
    </section>
  );
}
