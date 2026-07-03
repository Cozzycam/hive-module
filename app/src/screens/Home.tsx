import { useState, useMemo, useEffect } from 'react';
import { useColony } from '../state/colony';
import { usePins } from '../state/pins';
import { useTOD } from '../state/tod';
import { Card } from '../components/Card';
import { ConnectionDot } from '../components/ConnectionDot';
import { DigestCard } from '../components/DigestCard';
import { LiveNest } from '../components/LiveNest';
import { TopologyMiniMap } from '../components/TopologyMiniMap';
import { nameFromId } from '../data/plantNames';
import { deriveRoleTag } from '../data/personality';
import { sendCommand } from '../api/client';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import type { ColonyEvent } from '../api/types';

interface HomeProps {
  onNavigate: (tab: string, params?: Record<string, unknown>) => void;
}

export function Home({ onNavigate }: HomeProps) {
  const { snapshot, events, source, lastFetchMs, colonyId } = useColony();
  const { pins, isPinned, togglePin, pinName, rememberName } = usePins();
  const tod = useTOD(true);

  // Keep pinned names fresh from the roster (renames propagate; the cache
  // then outlives the roster when a followed conker dies)
  useEffect(() => {
    if (!snapshot?.lilguys) return;
    for (const id of pins) {
      const live = snapshot.lilguys.find(l => l.id === id);
      if (live?.name) rememberName(id, live.name);
    }
  }, [snapshot, pins, rememberName]);

  if (!snapshot) return null;

  const { population, food, modules, world } = snapshot;

  // Notable Now: from living roster
  const notableConkers = useMemo(() => {
    if (snapshot.lilguys) {
      return snapshot.lilguys.slice(0, 6);
    }
    return [];
  }, [snapshot]);

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
            {snapshot.colony_id || 'The Colony'}
          </h1>
          <div style={{ fontSize: SIZES.sm, color: tod.dimText, marginTop: 2 }}>
            {snapshot.queen_name ? <>Queen {snapshot.queen_name} &middot; </> : null}
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

      {/* While you were away */}
      <DigestCard palette={tod} />

      {/* The Nest — live view of the wee guys moving around */}
      <LiveNest
        snapshot={snapshot}
        palette={tod}
        onOpenCharacter={(id) => onNavigate('characters', { lilguyId: id })}
      />

      {/* Right Now tile */}
      <Card style={{ background: tod.cardBg }}>
        <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: tod.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
          Right Now
        </div>
        <RightNowContent population={population} food={food} tod={tod} />
        <CarePackageButton colonyId={colonyId} tod={tod} />
      </Card>

      {/* Field Guide — the colony's collection of visiting critters */}
      <Card
        style={{ background: tod.cardBg, display: 'flex', alignItems: 'center', gap: 12 }}
        onClick={() => onNavigate('fieldguide')}
      >
        <div style={{ fontSize: 26, minWidth: 34, textAlign: 'center' }}>{'\u{1F98B}'}</div>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: SIZES.base, fontWeight: 600, color: tod.text }}>
            Field Guide
          </div>
          <div style={{ fontSize: SIZES.sm, color: tod.dimText }}>
            Every visitor the colony has spotted
          </div>
        </div>
        <div style={{ fontSize: SIZES.base, color: tod.dimText }}>{'›'}</div>
      </Card>

      {/* A wish — one conker wants something the keeper can grant */}
      {snapshot.wish && (
        <WishCard wish={snapshot.wish} colonyId={colonyId} tod={tod} />
      )}

      {/* The Saga — the colony's story so far */}
      <Card
        style={{ background: tod.cardBg, display: 'flex', alignItems: 'center', gap: 12 }}
        onClick={() => onNavigate('saga')}
      >
        <div style={{ fontSize: 26, minWidth: 34, textAlign: 'center' }}>{'\u{1F4DC}'}</div>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: SIZES.base, fontWeight: 600, color: tod.text }}>
            The Saga
          </div>
          <div style={{ fontSize: SIZES.sm, color: tod.dimText }}>
            The colony{'’'}s story, as the chronicle tells it
          </div>
        </div>
        <div style={{ fontSize: SIZES.base, color: tod.dimText }}>{'›'}</div>
      </Card>

      {/* Gallery — what the makers have left in the world */}
      <Card
        style={{ background: tod.cardBg, display: 'flex', alignItems: 'center', gap: 12 }}
        onClick={() => onNavigate('gallery')}
      >
        <div style={{ fontSize: 26, minWidth: 34, textAlign: 'center' }}>{'\u{1F3A8}'}</div>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: SIZES.base, fontWeight: 600, color: tod.text }}>
            Gallery
          </div>
          <div style={{ fontSize: SIZES.sm, color: tod.dimText }}>
            Works the colony has made
          </div>
        </div>
        <div style={{ fontSize: SIZES.base, color: tod.dimText }}>{'›'}</div>
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
                {snapshot.lilguys?.find(l => l.id === id)?.name || pinName(id) || nameFromId(id)}
              </button>
            ))}
          </div>
        </>
      )}

      {/* Notable Now */}
      {notableConkers.length > 0 && (
        <>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: tod.dimText, textTransform: 'uppercase', letterSpacing: 1, margin: '16px 0 8px' }}>
            Notable Now
          </div>
          {notableConkers.map(l => (
            <Card
              key={l.id}
              style={{ background: tod.cardBg, display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}
              onClick={() => onNavigate('characters', { lilguyId: l.id })}
            >
              <div>
                <div style={{ fontSize: SIZES.base, fontWeight: 600, color: tod.text }}>
                  {l.name}
                </div>
                <div style={{ fontSize: SIZES.sm, color: tod.dimText }}>
                  {deriveRoleTag(l.personality, 'conker', l.traits || [])}
                </div>
              </div>
              <button
                onClick={(e) => { e.stopPropagation(); togglePin(l.id, l.name); }}
                style={{
                  background: 'none', border: 'none', cursor: 'pointer',
                  fontSize: 18, color: isPinned(l.id) ? HIVE.accent : HIVE.sand,
                  padding: 4,
                }}
                aria-label={isPinned(l.id) ? 'Unpin' : 'Pin'}
              >
                {isPinned(l.id) ? '\u2605' : '\u2606'}
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

// Care package: a day's food for the whole colony (sized by the queen,
// scales with the family), dropped near the chamber centre via the
// command queue. One per 6 hours — a gift, not a faucet.
const CARE_PKG_KEY = 'hive_last_care_pkg_ms';
const CARE_PKG_COOLDOWN_MS = 6 * 60 * 60 * 1000;

function CarePackageButton({ colonyId, tod }: {
  colonyId: string | null;
  tod: { text: string; dimText: string };
}) {
  const [state, setState] = useState<'ready' | 'sending' | 'sent' | 'cooldown'>(() => {
    const last = Number(localStorage.getItem(CARE_PKG_KEY) || 0);
    return Date.now() - last < CARE_PKG_COOLDOWN_MS ? 'cooldown' : 'ready';
  });

  const handleSend = async () => {
    if (!colonyId || state !== 'ready') return;
    setState('sending');
    const ok = await sendCommand(colonyId, 'feed_colony', { amount: 25 });
    if (ok) {
      localStorage.setItem(CARE_PKG_KEY, String(Date.now()));
      setState('sent');
    } else {
      setState('ready');
    }
  };

  const label = state === 'sending' ? 'Sending…'
    : state === 'sent' ? 'Package on its way \u{1F381}'
    : state === 'cooldown' ? 'Care package sent recently'
    : 'Send a care package \u{1F381}';

  return (
    <>
      <button
        onClick={handleSend}
        disabled={state !== 'ready'}
        style={{
          marginTop: 12, width: '100%', padding: '8px 0',
          background: 'none', border: `1px solid ${HIVE.sand}`,
          borderRadius: 16, fontSize: SIZES.sm,
          color: state === 'ready' ? tod.text : tod.dimText,
          cursor: state === 'ready' ? 'pointer' : 'default',
          opacity: state === 'cooldown' ? 0.6 : 1,
        }}
      >
        {label}
      </button>
      <div style={{ fontSize: SIZES.xs, color: tod.dimText, textAlign: 'center', marginTop: 4 }}>
        a day's food for the whole colony
      </div>
    </>
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

// A conker's wish, surfaced for the keeper — grant it and the kindness
// lands on that one wee guy (and in their story).
function WishCard({ wish, colonyId, tod }: {
  wish: { id: number; kind: 'treat' | 'boop'; name?: string };
  colonyId: string | null;
  tod: { cardBg: string; text: string; dimText: string };
}) {
  const [state, setState] = useState<'idle' | 'sending' | 'granted' | 'failed'>('idle');
  const name = wish.name || nameFromId(wish.id);
  const wants = wish.kind === 'treat' ? 'is craving a treat' : 'wants a boop';

  const grant = async () => {
    if (!colonyId || state === 'sending' || state === 'granted') return;
    setState('sending');
    const ok = await sendCommand(colonyId, 'grant_wish', { id: wish.id });
    setState(ok ? 'granted' : 'failed');
  };

  return (
    <Card style={{ background: '#F3E9D2', borderLeft: `3px solid ${HIVE.accent}`, display: 'flex', alignItems: 'center', gap: 12 }}>
      <div style={{ fontSize: 24, minWidth: 32, textAlign: 'center' }}>{'\u{1F4AD}'}</div>
      <div style={{ flex: 1 }}>
        <div style={{ fontSize: SIZES.base, fontWeight: 600, color: tod.text }}>
          {name} {wants}
        </div>
        <div style={{ fontSize: SIZES.xs, color: tod.dimText }}>
          {state === 'granted' ? 'Granted — they felt that. ✨'
            : state === 'failed' ? 'Couldn’t reach the colony — try again.'
            : 'A little kindness goes in the diary.'}
        </div>
      </div>
      {state !== 'granted' && (
        <button
          onClick={grant}
          disabled={state === 'sending'}
          style={{
            padding: '8px 14px', borderRadius: 10, border: 'none',
            background: HIVE.accent, color: HIVE.white, fontSize: SIZES.sm,
            fontWeight: 600, cursor: 'pointer', opacity: state === 'sending' ? 0.6 : 1,
          }}
        >
          {state === 'sending' ? '…' : 'Grant'}
        </button>
      )}
    </Card>
  );
}
