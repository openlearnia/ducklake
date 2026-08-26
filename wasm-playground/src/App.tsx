import { useEffect, useMemo, useState } from 'react';

import { DEMO_SEED_SQL, scenarios, type Scenario } from './demo/scenarios';
import { PlaygroundEngine, type QueryOutput } from './lib/engine';
import { ResultTable } from './components/ResultTable';
import { ScenarioRail } from './components/ScenarioRail';
import { SqlWorkspace } from './components/SqlWorkspace';
import { StatusPanel } from './components/StatusPanel';

type EngineStatus = 'starting' | 'ready' | 'error';

export default function App() {
  const engine = useMemo(() => new PlaygroundEngine(), []);
  const [selected, setSelected] = useState<Scenario>(scenarios[0]);
  const [sql, setSql] = useState(selected.sql);
  const [status, setStatus] = useState<EngineStatus>('starting');
  const [engineError, setEngineError] = useState<string | null>(null);
  const [output, setOutput] = useState<QueryOutput | null>(null);
  const [running, setRunning] = useState(false);

  useEffect(() => {
    let active = true;
    void engine.initialize().then(() => {
      if (active) setStatus('ready');
    }).catch((error: unknown) => {
      if (active) {
        setStatus('error');
        setEngineError(
          error instanceof Error
            ? `${error.message}\n[stack] ${error.stack ?? 'n/a'}`.slice(0, 1500)
            : String(error),
        );
      }
    });
    return () => {
      active = false;
      void engine.close();
    };
  }, [engine]);

  const chooseScenario = (scenario: Scenario) => {
    setSelected(scenario);
    setSql(scenario.sql);
    setOutput(null);
  };

  const runSql = async () => {
    setRunning(true);
    const result = await engine.run(sql);
    setOutput(result);
    setRunning(false);
  };

  const reset = async () => {
    setRunning(true);
    setOutput(null);
    try {
      await engine.reset();
      setStatus('ready');
      setEngineError(null);
    } catch (error: unknown) {
      setStatus('error');
      setEngineError(error instanceof Error ? error.message : String(error));
    } finally {
      setRunning(false);
    }
  };

  return (
    <main className="app-shell">
      <header className="topbar">
        <a className="brand" href="/" aria-label="DuckLake feature playground home"><span className="brand-mark">◒</span><span>DuckLake <em>playground</em></span></a>
        <div className="topbar-meta"><span className="live-mark" />Browser-native demo <span className="separator">/</span> custom build</div>
      </header>
      <div className="page-grid">
        <ScenarioRail disabled={status !== 'ready' || running} onSelect={chooseScenario} scenarios={scenarios} selectedId={selected.id} />
        <section className="content-column">
          <div className="hero">
            <div className="hero-copy">
              <div className="eyebrow">OpenLearnia × DuckDB 2.0 preview</div>
              <h1>Features that stay <span>close to the data.</span></h1>
              <p>Explore the custom DuckLake runtime we built for materialized views, refresh history, snapshots, and first-class JavaScript procedures — all inside this tab.</p>
            </div>
            <div className="hero-orbit" aria-hidden="true"><div className="orbit-ring ring-one" /><div className="orbit-ring ring-two" /><div className="orbit-core">DL</div><span className="orbit-label label-one">SQL</span><span className="orbit-label label-two">WASM</span><span className="orbit-label label-three">MV</span></div>
          </div>
          <StatusPanel error={engineError} status={status} variant={engine.variant} />
          <div className="scenario-heading"><div><div className="eyebrow">Selected scenario</div><h2>{selected.title}</h2></div><span className="focus-label">{selected.focus}</span></div>
          <SqlWorkspace disabled={status !== 'ready'} onChange={setSql} onReset={reset} onRun={runSql} running={running} sql={sql} />
          <ResultTable output={output} />
          <footer className="footer"><span>Seeded in memory · {DEMO_SEED_SQL.split('\n').filter((line) => line.trim()).length} setup statements</span><span>DuckLake feature preview</span></footer>
        </section>
      </div>
    </main>
  );
}
