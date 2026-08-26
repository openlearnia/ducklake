interface SqlWorkspaceProps {
  sql: string;
  onChange: (sql: string) => void;
  onRun: () => void;
  onReset: () => void;
  disabled: boolean;
  running: boolean;
}

export function SqlWorkspace({ sql, onChange, onRun, onReset, disabled, running }: SqlWorkspaceProps) {
  return (
    <section className="sql-workspace">
      <div className="workspace-toolbar">
        <div>
          <div className="eyebrow">SQL workspace</div>
          <p>Inspect or edit the scenario, then execute it in the browser.</p>
        </div>
        <div className="workspace-actions">
          <button className="button secondary" disabled={disabled || running} onClick={onReset} type="button">Reset</button>
          <button className="button primary" disabled={disabled || running} onClick={onRun} type="button">
            <span>{running ? 'Running…' : 'Run SQL'}</span><span aria-hidden="true">⌘↵</span>
          </button>
        </div>
      </div>
      <div className="editor-frame">
        <div className="editor-gutter" aria-hidden="true">{sql.split('\n').map((_, index) => <span key={index}>{String(index + 1).padStart(2, '0')}</span>)}</div>
        <textarea
          aria-label="SQL editor"
          disabled={disabled}
          onChange={(event) => onChange(event.target.value)}
          onKeyDown={(event) => {
            if ((event.metaKey || event.ctrlKey) && event.key === 'Enter') {
              event.preventDefault();
              onRun();
            }
          }}
          spellCheck={false}
          value={sql}
        />
      </div>
    </section>
  );
}
