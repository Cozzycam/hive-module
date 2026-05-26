import { useState } from 'react';
import { useColony } from '../state/colony';
import { useTOD } from '../state/tod';
import { Card } from '../components/Card';
import { TopologySchematic } from '../components/TopologySchematic';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import type { Module } from '../api/types';

export function Chambers() {
  const { snapshot } = useColony();
  const tod = useTOD(true);
  const [showPheromones, setShowPheromones] = useState(false);
  const [selectedModule, setSelectedModule] = useState<Module | null>(null);

  if (!snapshot) return null;

  const { modules, population } = snapshot;

  if (selectedModule) {
    return (
      <ModuleDetail
        module={selectedModule}
        allModules={modules}
        onBack={() => setSelectedModule(null)}
        palette={tod}
      />
    );
  }

  return (
    <div style={{ background: tod.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: tod.text, padding: '16px 0 8px' }}>
        Chambers
      </h1>
      <div style={{ fontSize: SIZES.sm, color: tod.dimText, marginBottom: 16 }}>
        {modules.length} module{modules.length !== 1 ? 's' : ''} &middot;{' '}
        {modules.filter(m => m.online).length} online
      </div>

      {/* Topology schematic */}
      <Card style={{ background: tod.cardBg }}>
        <div style={{
          display: 'flex', justifyContent: 'space-between', alignItems: 'center',
          marginBottom: 8,
        }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: tod.dimText, textTransform: 'uppercase', letterSpacing: 1 }}>
            Colony Layout
          </div>
          <button
            onClick={() => setShowPheromones(!showPheromones)}
            style={{
              background: showPheromones ? HIVE.accent : 'transparent',
              color: showPheromones ? HIVE.white : tod.dimText,
              border: `1px solid ${showPheromones ? HIVE.accent : HIVE.sand}`,
              borderRadius: 16, padding: '3px 10px', fontSize: SIZES.xs,
              cursor: 'pointer',
            }}
          >
            Pheromones
          </button>
        </div>
        <TopologySchematic
          modules={modules}
          showPheromones={showPheromones}
          onModuleClick={setSelectedModule}
        />
      </Card>

      {/* Module list */}
      {modules.map(m => (
        <Card
          key={m.id}
          style={{ background: tod.cardBg }}
          onClick={() => setSelectedModule(m)}
        >
          <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
            <div>
              <div style={{ fontSize: SIZES.base, fontWeight: 600, color: tod.text }}>
                {m.role === 'queen' ? 'Queen Module' : `Satellite ${m.id}`}
              </div>
              <div style={{ fontSize: SIZES.sm, color: tod.dimText }}>
                {m.id} &middot; {m.online ? 'online' : 'offline'}
              </div>
            </div>
            <div style={{
              width: 8, height: 8, borderRadius: '50%',
              background: m.online ? HIVE.green : HIVE.dimText,
            }} />
          </div>
        </Card>
      ))}
    </div>
  );
}

// ---- Module Detail ----

function ModuleDetail({ module, allModules, onBack, palette }: {
  module: Module;
  allModules: Module[];
  onBack: () => void;
  palette: { bg: string; cardBg: string; text: string; dimText: string };
}) {
  const faceEntries = Object.entries(module.faces).filter(([, v]) => v);

  return (
    <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <div style={{ padding: '16px 0 8px' }}>
        <button onClick={onBack} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: SIZES.base, color: HIVE.accent }}>
          &larr; Back
        </button>
      </div>

      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: palette.text, margin: '0 0 4px' }}>
        {module.role === 'queen' ? 'Queen Module' : `Satellite ${module.id}`}
      </h1>
      <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginBottom: 16 }}>
        ID: {module.id} &middot; {module.online ? 'Online' : 'Offline'}
      </div>

      {/* Face connections */}
      <Card style={{ background: palette.cardBg }}>
        <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
          Connections
        </div>
        {faceEntries.length === 0 ? (
          <div style={{ color: palette.dimText, fontSize: SIZES.sm }}>No connections</div>
        ) : (
          faceEntries.map(([face, neighborId]) => {
            const neighbor = allModules.find(m => m.id === neighborId);
            return (
              <div key={face} style={{ display: 'flex', justifyContent: 'space-between', padding: '4px 0', fontSize: SIZES.sm }}>
                <span style={{ color: palette.dimText, textTransform: 'capitalize' }}>{face}</span>
                <span style={{ color: palette.text, fontWeight: 500 }}>
                  {neighbor?.role === 'queen' ? 'Queen' : neighborId} {neighbor?.online ? '' : '(offline)'}
                </span>
              </div>
            );
          })
        )}
      </Card>
    </div>
  );
}
