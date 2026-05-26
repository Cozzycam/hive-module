import { useState, useMemo, useEffect } from 'react';
import { useColony } from '../state/colony';
import { usePins } from '../state/pins';
import { useTOD } from '../state/tod';
import { Card } from '../components/Card';
import { ConnectionDot } from '../components/ConnectionDot';
import { TopologyMiniMap } from '../components/TopologyMiniMap';
import { nameFromId } from '../data/plantNames';
import { deriveRoleTag } from '../data/personality';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import type { ColonyEvent } from '../api/types';

interface HomeProps {
  onNavigate: (tab: string, params?: Record<string, unknown>) => void;
}

export function Home({ onNavigate }: HomeProps) {
  const { snapshot, events, source, lastFetchMs } = useColony();
  const { pins, isPinned, togglePin } = usePins();
  const tod = useTOD(true);

  if (!snapshot) return null;

  const { population, food, modules, world } = snapshot;

  // Notable Now: recently active lilguys, filtered to living roster
  const notableIds = useMemo(() => {
    const rosterIds = new Set(snapshot.lilguys?.map(l => l.id) ?? []);
    const seen = new Set<number>();
    const ids: number[] = [];
    for (let i = events.length - 1; i >= 0 && ids.length < 6; i--) {
      const e = events[i];
      if (e.lilguy && !seen.has(e.lilguy) && (rosterIds.size === 0 || rosterIds.has(e.lilguy))) {
        seen.add(e.lilguy);
        ids.push(e.lilguy);
      }
    }
    return ids;
  }, [events, snapshot]);

  // Weather description
  const weatherLabel = world.weather.replace(/_/g, ' ');
  const seasonLabel = world.season.charAt(0).toUpperCase() + world.season.slice(1);

  return (
    <div style={{ background: tod.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      {/* Header */}
      <div style={{
        display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        padding: '16px 0 12px',
      }}>
        <div>
          <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: tod.text, margin: 0 }}>
            The Colony
          </h1>
          <div style={{ fontSize: SIZES.sm, color: tod.dimText, marginTop: 2 }}>
            {seasonLabel} &middot; {weatherLabel} &middot; {world.tod.phase}
          </div>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
          <ConnectionDot />
          <button
            onClick={() => onNavigate('settings')}
            style={{
              background: 'none', border: 'none', cursor: 'pointer',
              fontSize: 18, color: tod.dimText, padding: 4,
            }}
            aria-label="Settings"
          >
            &#9881;
          </button>
        </div>
      </div>

      {/* Right Now tile */}
      <Card style={{ background: tod.cardBg }}>
        <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: tod.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
          Right Now
        </div>
        <RightNowContent population={population} food={food} tod={tod} />
      </Card>

      {/* Topology mini-map */}
      {modules.length > 0 && (
        <Card style={{ background: tod.cardBg, display: 'flex', alignItems: 'center', gap: 16 }}
              onClick={() => onNavigate('chambers')}>
          <TopologyMiniMap modules={modules} size={80} />
          <div>
            <div style={{ fontSize: SIZES.base, fontWeight: 600, color: tod.text }}>
              {modules.length} module{modules.length !== 1 ? 's' : ''}
            </div>
            <div style={{ fontSize: SIZES.sm, color: tod.dimText }}>
              {modules.filter(m => m.online).length} online
            </div>
          </div>
        </Card>
      )}

      {/* Active challenges */}
      {world.active_challenges.length > 0 && (
        <Card style={{ background: '#F5E6D0', borderLeft: `3px solid ${HIVE.alert}` }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: HIVE.alert, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 4 }}>
            Active Challenge
          </div>
          {world.active_challenges.map((c, i) => (
            <div key={i} style={{ fontSize: SIZES.base, color: HIVE.ink }}>
              {c.type.replace(/_/g, ' ')} &middot; severity {(c.severity * 100).toFixed(0)}%
            </div>
          ))}
        </Card>
      )}

      {/* Following (pinned) */}
      {pins.length > 0 && (
        <>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: tod.dimText, textTransform: 'uppercase', letterSpacing: 1, margin: '16px 0 8px' }}>
            Following
          </div>
          <div style={{ display: 'flex', gap: 8, overflowX: 'auto', paddingBottom: 4 }}>
            {pins.map(id => (
              <button
                key={id}
                onClick={() => onNavigate('characters', { lilguyId: id })}
                style={{
                  background: tod.cardBg, border: 'none', borderRadius: 12,
                  padding: '8px 14px', cursor: 'pointer', whiteSpace: 'nowrap',
                  fontSize: SIZES.sm, fontWeight: 500, color: tod.text,
                }}
              >
                {nameFromId(id)}
              </button>
            ))}
          </div>
        </>
      )}

      {/* Notable Now */}
      {notableIds.length > 0 && (
        <>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: tod.dimText, textTransform: 'uppercase', letterSpacing: 1, margin: '16px 0 8px' }}>
            Notable Now
          </div>
          {notableIds.map(id => (
            <Card
              key={id}
              style={{ background: tod.cardBg, display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}
              onClick={() => onNavigate('characters', { lilguyId: id })}
            >
              <div>
                <div style={{ fontSize: SIZES.base, fontWeight: 600, color: tod.text }}>
                  {nameFromId(id)}
                </div>
                <div style={{ fontSize: SIZES.sm, color: tod.dimText }}>
                  {deriveRoleTag(undefined, 'worker', [])}
                </div>
              </div>
              <button
                onClick={(e) => { e.stopPropagation(); togglePin(id); }}
                style={{
                  background: 'none', border: 'none', cursor: 'pointer',
                  fontSize: 18, color: isPinned(id) ? HIVE.accent : HIVE.sand,
                  padding: 4,
                }}
                aria-label={isPinned(id) ? 'Unpin' : 'Pin'}
              >
                {isPinned(id) ? '\u2605' : '\u2606'}
              </button>
            </Card>
          ))}
        </>
      )}
    </div>
  );
}

// Sub-component: Right Now stats
function RightNowContent({ population, food, tod }: {
  population: { alive: number; by_role: Record<string, number> };
  food: { store: number; daily_burn: number; days_remaining: number };
  tod: { text: string; dimText: string };
}) {
  // TODO: Replace placeholder animation with real worker positions
  // when snapshot includes per-lilguy coordinates
  const [tick, setTick] = useState(0);
  useEffect(() => {
    const t = setInterval(() => setTick(v => v + 1), 2000);
    return () => clearInterval(t);
  }, []);

  return (
    <div style={{ display: 'flex', gap: 24 }}>
      <div style={{ flex: 1 }}>
        <StatRow label="Population" value={population.alive} color={tod.text} dimColor={tod.dimText} />
        <StatRow label="Workers" value={population.by_role.conker} color={tod.text} dimColor={tod.dimText} />
        <StatRow label="Brood" value={population.by_role.brood_egg + population.by_role.brood_seed} color={tod.text} dimColor={tod.dimText} />
      </div>
      <div style={{ flex: 1 }}>
        <StatRow label="Food" value={`${food.store.toFixed(0)}u`} color={tod.text} dimColor={tod.dimText} />
        <StatRow label="Burn rate" value={`${food.daily_burn.toFixed(1)}/d`} color={tod.text} dimColor={tod.dimText} />
        <StatRow
          label="Reserves"
          value={food.days_remaining >= 999 ? '\u221e' : `${food.days_remaining.toFixed(1)}d`}
          color={food.days_remaining < 2 ? HIVE.alert : tod.text}
          dimColor={tod.dimText}
        />
      </div>
    </div>
  );
}

function StatRow({ label, value, color, dimColor }: {
  label: string; value: string | number; color: string; dimColor: string;
}) {
  return (
    <div style={{ marginBottom: 6 }}>
      <div style={{ fontSize: SIZES.xs, color: dimColor }}>{label}</div>
      <div style={{ fontSize: SIZES.md, fontWeight: 600, color }}>{value}</div>
    </div>
  );
}
