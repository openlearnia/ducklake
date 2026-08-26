import type { Scenario, ScenarioId } from '../demo/scenarios';

interface ScenarioRailProps {
  scenarios: readonly Scenario[];
  selectedId: ScenarioId;
  onSelect: (scenario: Scenario) => void;
  disabled: boolean;
}

export function ScenarioRail({ scenarios, selectedId, onSelect, disabled }: ScenarioRailProps) {
  return (
    <aside className="scenario-rail" aria-label="Feature scenarios">
      <div className="eyebrow">Feature index</div>
      <h2>What we added</h2>
      <p className="rail-intro">Run the same SQL in the custom browser build. Every card is backed by a real DuckLake operation.</p>
      <nav>
        {scenarios.map((scenario, index) => (
          <button
            className={`scenario-card ${scenario.id === selectedId ? 'selected' : ''}`}
            disabled={disabled}
            key={scenario.id}
            onClick={() => onSelect(scenario)}
            type="button"
          >
            <span className="scenario-number">0{index + 1}</span>
            <span className="scenario-copy">
              <strong>{scenario.title}</strong>
              <span>{scenario.summary}</span>
            </span>
            <span className="scenario-arrow" aria-hidden="true">↗</span>
          </button>
        ))}
      </nav>
      <div className="rail-note">
        <span className="note-mark">⌁</span>
        <span>Local-first by design. No warehouse credentials leave this page.</span>
      </div>
    </aside>
  );
}
