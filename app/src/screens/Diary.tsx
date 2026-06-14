import { useState, useMemo, useEffect, useCallback } from 'react';
import { useColony, useColonyActions } from '../state/colony';
import { Card } from '../components/Card';
import { Chip } from '../components/Chip';
import { nameFromId } from '../data/plantNames';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import type { ColonyEvent, EventType } from '../api/types';

const EVENT_CATEGORIES: { label: string; types: EventType[] }[] = [
  { label: 'All', types: [] },
  { label: 'Births', types: ['hatch'] },
  { label: 'Deaths', types: ['death'] },
  { label: 'Milestones', types: ['milestone', 'colony_event'] },
  { label: 'Social', types: ['bond_formed', 'bond_broken', 'tended_by_assigned', 'play'] },
  { label: 'Discoveries', types: ['discovery'] },
  { label: 'Challenges', types: ['challenge_start', 'challenge_end'] },
  { label: 'Traits', types: ['trait_earned', 'role_change'] },
];

export function Diary() {
  const { events, colonyId, snapshot } = useColony();
  const { refreshEvents } = useColonyActions();
  const palette = TOD_PALETTES.day; // Reflective — always day

  const [categoryIdx, setCategoryIdx] = useState(0);
  const [refreshing, setRefreshing] = useState(false);

  // Fetch events on mount
  useEffect(() => {
    refreshEvents();
  }, []);

  const filtered = useMemo(() => {
    const cat = EVENT_CATEGORIES[categoryIdx];
    // Filter out noisy chamber_crossing events from diary
    const meaningful = events.filter(e => e.type !== 'chamber_crossing');
    const list = cat.types.length === 0
      ? meaningful
      : meaningful.filter(e => cat.types.includes(e.type));
    // Group food_delivered events within 10 minutes of each other
    const grouped: (ColonyEvent | { type: 'food_group'; count: number; totalAmount: number; unix: number; tick: number; lilguy: number; data: Record<string, unknown> } | { type: 'play_group'; count: number; unix: number; tick: number; lilguy: number; data: Record<string, unknown> })[] = [];
    const TEN_MIN = 600;
    const THIRTY_MIN = 1800;
    for (let i = 0; i < list.length; i++) {
      // Collapse runs of play — conkers romp constantly; one "playing" note per
      // burst, not one per romp.
      if (list[i].type === 'play') {
        let count = 1;
        const startUnix = list[i].unix;
        const firstName = (list[i] as unknown as { name?: string }).name;
        while (i + 1 < list.length && list[i + 1].type === 'play'
               && Math.abs(list[i + 1].unix - startUnix) < THIRTY_MIN) {
          i++; count++;
        }
        if (count === 1) {
          grouped.push(list[i]);
        } else {
          grouped.push({
            type: 'play_group', count,
            unix: startUnix, tick: list[i].tick, lilguy: 0, data: { name: firstName },
          });
        }
        continue;
      }
      if (list[i].type !== 'food_delivered') {
        grouped.push(list[i]);
        continue;
      }
      let count = 1;
      let total = ((list[i].data as { amount?: number }).amount) || 0;
      const startUnix = list[i].unix;
      while (i + 1 < list.length && list[i + 1].type === 'food_delivered'
             && Math.abs(list[i + 1].unix - startUnix) < TEN_MIN) {
        i++;
        count++;
        total += ((list[i].data as { amount?: number }).amount) || 0;
      }
      if (count === 1) {
        grouped.push(list[i]);
      } else {
        grouped.push({
          type: 'food_group', count, totalAmount: total,
          unix: startUnix, tick: list[i].tick, lilguy: 0, data: {},
        });
      }
    }
    return [...grouped].reverse(); // newest first
  }, [events, categoryIdx]);

  // Build name lookup from roster (firmware random names)
  const rosterNames = useMemo(() => {
    const map = new Map<number, string>();
    if (snapshot?.lilguys) {
      for (const l of snapshot.lilguys) map.set(l.id, l.name);
    }
    return map;
  }, [snapshot]);

  const handleRefresh = useCallback(async () => {
    setRefreshing(true);
    await refreshEvents();
    setRefreshing(false);
  }, [refreshEvents]);

  return (
    <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '16px 0 8px' }}>
        <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: palette.text, margin: 0 }}>
          Diary
        </h1>
        <button
          onClick={handleRefresh}
          disabled={refreshing}
          style={{
            background: 'none', border: `1px solid ${HIVE.sand}`,
            borderRadius: 16, padding: '4px 12px', fontSize: SIZES.xs,
            color: palette.dimText, cursor: 'pointer',
            opacity: refreshing ? 0.5 : 1,
          }}
        >
          {refreshing ? 'Refreshing...' : 'Refresh'}
        </button>
      </div>

      {/* Category filters */}
      <div style={{ display: 'flex', gap: 6, overflowX: 'auto', paddingBottom: 8, marginBottom: 8 }}>
        {EVENT_CATEGORIES.map((cat, i) => (
          <Chip
            key={cat.label}
            label={cat.label}
            active={categoryIdx === i}
            onClick={() => setCategoryIdx(i)}
          />
        ))}
      </div>

      {/* Events list */}
      {filtered.length === 0 ? (
        <div style={{ color: palette.dimText, fontSize: SIZES.base, textAlign: 'center', padding: 32 }}>
          No events to show
        </div>
      ) : (
        filtered.map((ev, i) => (
          <DiaryEntry key={`${ev.unix}-${ev.tick}-${i}`} event={ev as ColonyEvent} palette={palette} rosterNames={rosterNames} />
        ))
      )}
    </div>
  );
}

function DiaryEntry({ event, palette, rosterNames }: {
  event: ColonyEvent;
  palette: { cardBg: string; text: string; dimText: string };
  rosterNames: Map<number, string>;
}) {
  const date = new Date(event.unix * 1000);
  const dateStr = date.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
  const timeStr = date.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });

  const { icon, description } = formatEvent(event, rosterNames);

  return (
    <Card style={{ background: palette.cardBg, padding: 12 }}>
      <div style={{ display: 'flex', gap: 10 }}>
        <div style={{ fontSize: 20, lineHeight: '24px', minWidth: 28, textAlign: 'center' }}>
          {icon}
        </div>
        <div style={{ flex: 1 }}>
          <div style={{ fontSize: SIZES.sm, color: palette.text }}>{description}</div>
          <div style={{ fontSize: SIZES.xs, color: palette.dimText, marginTop: 2 }}>
            {dateStr} {timeStr}
          </div>
        </div>
      </div>
    </Card>
  );
}

function formatEvent(ev: ColonyEvent, rosterNames: Map<number, string>): { icon: string; description: string } {
  const data = ev.data as Record<string, unknown>;
  // Name priority: embedded in the event (survives death) > live roster > wordlist fallback
  const embedded = (ev as unknown as { name?: string }).name;
  const name = ev.lilguy
    ? (embedded || rosterNames.get(ev.lilguy) || nameFromId(ev.lilguy))
    : 'Colony';
  const targetName = (id: unknown): string =>
    (data.target_name as string)
    || (typeof id === 'number' ? (rosterNames.get(id) || nameFromId(id)) : 'another');

  switch (ev.type) {
    case 'hatch':
      return {
        icon: '\u{1F331}',
        description: `${name} was born${data.is_pioneer ? ' — a founder' : ''}.`,
      };
    case 'death':
      return {
        icon: '\u{1F342}',
        description: `${name} passed away. ${data.cause === 'starvation' ? 'Cause: starvation.' : ''}`,
      };
    case 'role_change':
      return {
        icon: '\u{1F451}',
        description: `${name} changed role.`,
      };
    case 'bond_formed':
      return {
        icon: '\u{1F91D}',
        description: `${name} formed a bond with ${targetName(data.target_id)}.`,
      };
    case 'bond_broken':
      return {
        icon: '\u{1F494}',
        description: `${name}'s bond with ${targetName(data.target_id)} faded.`,
      };
    case 'milestone':
      return {
        icon: '\u{2B50}',
        description: `Colony milestone: ${String(data.kind).replace(/_/g, ' ')} (${data.value}).`,
      };
    case 'colony_event':
      return {
        icon: '\u{1F3E0}',
        description: `Colony: ${String(data.kind).replace(/_/g, ' ')}.`,
      };
    case 'discovery': {
      const critter = String(data.critter || 'critter');
      const icon = critter === 'butterfly' ? '\u{1F98B}'
                 : critter === 'worm' ? '\u{1FAB1}'
                 : '\u{1FAB2}'; // beetle
      return {
        icon,
        description: `${name} found a ${critter}!`,
      };
    }
    case 'challenge_start':
      return {
        icon: '\u{26A0}',
        description: `${String(data.type).replace(/_/g, ' ')} began (severity ${((data.severity as number) * 100).toFixed(0)}%).`,
      };
    case 'challenge_end':
      return {
        icon: '\u{2600}',
        description: `${String(data.type).replace(/_/g, ' ')} ended.`,
      };
    case 'trait_earned':
      return {
        icon: '\u{1F3C5}',
        description: `${name} earned the "${data.trait}" trait.`,
      };
    case 'tended_by_assigned':
      return {
        icon: '\u{1F9B7}',
        description: `${name} was assigned a caretaker${data.carer_id ? `: ${rosterNames.get(data.carer_id as number) || nameFromId(data.carer_id as number)}` : ''}.`,
      };
    case 'mourning' as EventType:
      return {
        icon: '\u{1F56F}\u{FE0F}',
        description: `${name} stood vigil for a fallen friend.`,
      };
    case 'play' as EventType:
      return {
        icon: '\u{1F389}',
        description: `${name} led a parade — ${Math.max(0, ((data.participants as number) || 2) - 1)} joined in.`,
      };
    case 'food_group' as EventType:
      return {
        icon: '\u{1F36F}',
        description: `${(ev as unknown as { count: number }).count} food deliveries (${((ev as unknown as { totalAmount: number }).totalAmount).toFixed(0)}u total).`,
      };
    case 'play_group' as EventType:
      return {
        icon: '\u{2728}',
        description: `Lots of playing — ${(ev as unknown as { count: number }).count} romps.`,
      };
    default:
      return {
        icon: '\u{1F4AC}',
        description: `${name}: ${ev.type.replace(/_/g, ' ')}.`,
      };
  }
}
