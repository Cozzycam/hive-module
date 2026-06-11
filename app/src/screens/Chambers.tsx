import { useState } from 'react';
import { useColony } from '../state/colony';
import { useTOD } from '../state/tod';
import { Card } from '../components/Card';
import { TopologySchematic } from '../components/TopologySchematic';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { sendCommand, getStoredColonyId } from '../api/client';
import type { Module, ModuleRole } from '../api/types';

// Display metadata per role. Every module is identical hardware — the role
// is assigned here and the queen relays it over ESP-NOW.
const ROLE_META: Record<ModuleRole, { label: string; icon: string; blurb: string }> = {
  queen: { label: 'Queen Module', icon: '👑', blurb: 'Heart of the colony — source of truth.' },
  satellite: { label: 'Satellite', icon: '🛰️', blurb: 'Plain extension chamber.' },
  garden: { label: 'Garden', icon: '🌿', blurb: 'A growing place. Behaviours coming soon.' },
  food_store: { label: 'Food Storage', icon: '🌰', blurb: 'Deep larder. Behaviours coming soon.' },
  heart_tree: { label: 'Heart Tree', icon: '🌳', blurb: 'The colony’s living memory. Behaviours coming soon.' },
};

const ASSIGNABLE_ROLES: ModuleRole[] = ['satellite', 'garden', 'food_store', 'heart_tree'];

// Ground tints — hue presets the firmware scales the sand palette toward.
// null colour = reset to the default sand.
const TINTS: { name: string; color: string | null }[] = [
  { name: 'Sand', color: null },
  { name: 'Clay', color: '#C4744A' },
  { name: 'Moss', color: '#7AA05A' },
  { name: 'Honey', color: '#D9A441' },
  { name: 'Slate', color: '#7A8A99' },
  { name: 'Lavender', color: '#A98AC4' },
  { name: 'Rose', color: '#C47A8A' },
];

function moduleTitle(m: Module): string {
  const meta = ROLE_META[m.role] ?? ROLE_META.satellite;
  return m.role === 'queen' ? meta.label : `${meta.label} ${m.id}`;
}

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
                {(ROLE_META[m.role] ?? ROLE_META.satellite).icon} {moduleTitle(m)}
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
  const [queuedRole, setQueuedRole] = useState<ModuleRole | null>(null);
  const [sendFailed, setSendFailed] = useState(false);
  const [queuedTint, setQueuedTint] = useState<string | null | undefined>(undefined);

  const assignRole = async (role: ModuleRole) => {
    if (role === module.role || queuedRole) return;
    setSendFailed(false);
    const colonyId = getStoredColonyId();
    if (!colonyId) { setSendFailed(true); return; }
    const ok = await sendCommand(colonyId, 'set_module_role', { module: module.id, role });
    if (ok) setQueuedRole(role);
    else setSendFailed(true);
  };

  const assignTint = async (color: string | null) => {
    const colonyId = getStoredColonyId();
    if (!colonyId) return;
    const ok = await sendCommand(colonyId, 'set_floor_tint', {
      module: module.id, color: color ?? '',
    });
    if (ok) setQueuedTint(color);
  };

  const currentTint = queuedTint !== undefined
    ? queuedTint
    : (module.tint ? module.tint.toUpperCase() : null);

  return (
    <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <div style={{ padding: '16px 0 8px' }}>
        <button onClick={onBack} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: SIZES.base, color: HIVE.accent }}>
          &larr; Back
        </button>
      </div>

      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: palette.text, margin: '0 0 4px' }}>
        {(ROLE_META[module.role] ?? ROLE_META.satellite).icon} {moduleTitle(module)}
      </h1>
      <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginBottom: 16 }}>
        ID: {module.id} &middot; {module.online ? 'Online' : 'Offline'}
      </div>

      {/* Role assignment — every module is identical; the role is just data.
          The queen's own role can't be changed from the app. */}
      {module.role !== 'queen' && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
            Role
          </div>
          <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
            {ASSIGNABLE_ROLES.map(role => {
              const meta = ROLE_META[role];
              const current = role === (queuedRole ?? module.role);
              return (
                <button
                  key={role}
                  onClick={() => assignRole(role)}
                  disabled={queuedRole !== null}
                  style={{
                    background: current ? HIVE.accent : 'transparent',
                    color: current ? HIVE.white : palette.text,
                    border: `1px solid ${current ? HIVE.accent : HIVE.sand}`,
                    borderRadius: 16, padding: '6px 12px', fontSize: SIZES.sm,
                    cursor: queuedRole ? 'default' : 'pointer',
                    opacity: queuedRole && !current ? 0.5 : 1,
                  }}
                >
                  {meta.icon} {meta.label}
                </button>
              );
            })}
          </div>
          <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginTop: 8 }}>
            {queuedRole
              ? `Role change queued — the queen applies it within ~30s.`
              : sendFailed
                ? 'Couldn’t reach the colony — try again in a moment.'
                : (ROLE_META[module.role] ?? ROLE_META.satellite).blurb}
          </div>
        </Card>
      )}

      {/* Ground tint — any module, applied on the device within ~30s */}
      <Card style={{ background: palette.cardBg }}>
        <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
          Ground Tint
        </div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 12 }}>
          {TINTS.map(t => {
            const selected = (t.color === null && !currentTint)
              || (t.color !== null && currentTint === t.color.toUpperCase());
            return (
              <button
                key={t.name}
                onClick={() => assignTint(t.color)}
                style={{ background: 'none', border: 'none', cursor: 'pointer', padding: 0, textAlign: 'center' }}
                aria-label={t.name}
              >
                <div style={{
                  width: 38, height: 38, borderRadius: '50%',
                  background: t.color ?? '#E8D4A8',
                  border: selected ? `3px solid ${HIVE.accent}` : `2px solid ${HIVE.sand}`,
                  boxSizing: 'border-box',
                }} />
                <div style={{ fontSize: SIZES.xs, color: selected ? HIVE.accent : palette.dimText, marginTop: 4 }}>
                  {t.name}
                </div>
              </button>
            );
          })}
        </div>
        {queuedTint !== undefined && (
          <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginTop: 10 }}>
            Tint queued — the ground changes within ~30s.
          </div>
        )}
      </Card>

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
