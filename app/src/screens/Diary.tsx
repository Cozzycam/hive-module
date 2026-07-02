import { useState, useMemo, useEffect, useCallback } from 'react';
import { useColony, useColonyActions } from '../state/colony';
import { Card } from '../components/Card';
import { Chip } from '../components/Chip';
import { nameFromId } from '../data/plantNames';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import type { ColonyEvent, EventType } from '../api/types';

// Rollup entries synthesized by the chronicle view — one line per kind
// of ambient hum per day
type Rollup = {
  type: 'food_group' | 'play_group' | 'discovery_group' | 'tap_group' | 'flicker_group';
  count: number;
  totalAmount?: number;
  unix: number;
  tick: number;
  lilguy: number;
  data: Record<string, unknown>;
};
type DiaryItem = ColonyEvent | Rollup;

// Neighbour connect/disconnect churn — a flapping pogo connection can log
// dozens of these a day; the chronicle shows one flicker line instead
const FLICKER_KINDS = new Set([
  'module_connected', 'module_disconnected', 'met_a_neighbouring_kingdom',
]);

function dayLabel(unix: number): string {
  const d = new Date(unix * 1000);
  const today = new Date();
  const yesterday = new Date();
  yesterday.setDate(today.getDate() - 1);
  if (d.toDateString() === today.toDateString()) return 'Today';
  if (d.toDateString() === yesterday.toDateString()) return 'Yesterday';
  return d.toLocaleDateString(undefined, { weekday: 'short', day: 'numeric', month: 'short' });
}

const EVENT_CATEGORIES: { label: string; types: EventType[] }[] = [
  { label: 'All', types: [] },
  { label: 'Births', types: ['hatch'] },
  { label: 'Deaths', types: ['death'] },
  { label: 'Milestones', types: ['milestone', 'colony_event'] },
  { label: 'Social', types: ['bond_formed', 'became_best_friends', 'bond_broken', 'tended_by_assigned', 'play'] },
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

  const sections = useMemo(() => {
    const cat = EVENT_CATEGORIES[categoryIdx];
    // Filter out noisy chamber_crossing events from diary
    const meaningful = events.filter(e => e.type !== 'chamber_crossing');

    // Drill-down categories: raw entries, with the old run-grouping so
    // Social/Discoveries stay readable
    if (cat.types.length > 0) {
      const list = meaningful.filter(e => cat.types.includes(e.type));
      const grouped: DiaryItem[] = [];
      const TEN_MIN = 600;
      const THIRTY_MIN = 1800;
      for (let i = 0; i < list.length; i++) {
        if (list[i].type === 'play') {
          let count = 1;
          const startUnix = list[i].unix;
          const firstName = (list[i] as unknown as { name?: string }).name;
          while (i + 1 < list.length && list[i + 1].type === 'play'
                 && Math.abs(list[i + 1].unix - startUnix) < THIRTY_MIN) {
            i++; count++;
          }
          if (count === 1) grouped.push(list[i]);
          else grouped.push({
            type: 'play_group', count,
            unix: startUnix, tick: list[i].tick, lilguy: 0, data: { name: firstName },
          });
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
        if (count === 1) grouped.push(list[i]);
        else grouped.push({
          type: 'food_group', count, totalAmount: total,
          unix: startUnix, tick: list[i].tick, lilguy: 0, data: {},
        });
      }
      return [{ label: null as string | null, items: [...grouped].reverse() }];
    }

    // "All": the chronicle. Days become sections; headline moments render
    // individually while the ambient hum (discoveries, romps, hauls, tunnel
    // flicker, keeper feeds) rolls up to one line per kind per day.
    // (Amber: "too many parade entries... 90% of what's on the diary")
    const byDay = new Map<string, ColonyEvent[]>();
    for (const e of meaningful) {
      const k = new Date(e.unix * 1000).toDateString();
      if (!byDay.has(k)) byDay.set(k, []);
      byDay.get(k)!.push(e);
    }

    const out: { label: string | null; items: DiaryItem[] }[] = [];
    for (const dayEvents of byDay.values()) {
      const items: DiaryItem[] = [];
      const counts: Record<string, number> = {};
      let discLast = 0, discTick = 0;
      let plays = 0, playLast = 0, playTick = 0;
      let foods = 0, foodTotal = 0, foodLast = 0, foodTick = 0;
      let taps = 0, tapLast = 0, tapTick = 0;
      let flickers = 0, flickLast = 0, flickTick = 0;

      for (const e of dayEvents) {
        const kind = String((e.data as { kind?: string })?.kind ?? '');
        if (e.type === 'discovery') {
          const c = String((e.data as { critter?: string }).critter || 'critter');
          counts[c] = (counts[c] || 0) + 1;
          discLast = e.unix; discTick = e.tick;
        } else if (e.type === 'play') {
          plays++; playLast = e.unix; playTick = e.tick;
        } else if (e.type === 'food_delivered') {
          foods++;
          foodTotal += Number((e.data as { amount?: number }).amount) || 0;
          foodLast = e.unix; foodTick = e.tick;
        } else if (e.type === 'food_tap') {
          taps++; tapLast = e.unix; tapTick = e.tick;
        } else if (e.type === 'food_discovered' || e.type === 'tended_by_assigned') {
          // quiet ambience — visible under their filters, not the chronicle
        } else if (e.type === 'colony_event' && FLICKER_KINDS.has(kind)) {
          flickers++; flickLast = e.unix; flickTick = e.tick;
        } else {
          items.push(e);
        }
      }

      if (Object.keys(counts).length > 0) {
        items.push({
          type: 'discovery_group',
          count: Object.values(counts).reduce((a, b) => a + b, 0),
          unix: discLast, tick: discTick, lilguy: 0, data: { counts },
        });
      }
      if (plays > 0) items.push({ type: 'play_group', count: plays, unix: playLast, tick: playTick, lilguy: 0, data: {} });
      if (foods > 0) items.push({ type: 'food_group', count: foods, totalAmount: foodTotal, unix: foodLast, tick: foodTick, lilguy: 0, data: {} });
      if (taps > 0) items.push({ type: 'tap_group', count: taps, unix: tapLast, tick: tapTick, lilguy: 0, data: {} });
      if (flickers > 0) items.push({ type: 'flicker_group', count: flickers, unix: flickLast, tick: flickTick, lilguy: 0, data: {} });

      items.sort((a, b) => b.unix - a.unix);
      out.push({ label: dayLabel(dayEvents[0].unix), items });
    }
    return out.reverse(); // newest day first
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

      {/* Chronicle */}
      {sections.every(s => s.items.length === 0) ? (
        <div style={{ color: palette.dimText, fontSize: SIZES.base, textAlign: 'center', padding: 32 }}>
          No events to show
        </div>
      ) : (
        sections.map((section, si) => (
          <div key={section.label ?? si}>
            {section.label && (
              <div style={{
                fontSize: SIZES.xs, fontWeight: 700, color: palette.dimText,
                textTransform: 'uppercase', letterSpacing: 1,
                padding: '14px 4px 6px',
              }}>
                {section.label}
              </div>
            )}
            {section.items.map((ev, i) => (
              <DiaryEntry key={`${ev.unix}-${ev.tick}-${i}`} event={ev as ColonyEvent} palette={palette} rosterNames={rosterNames} />
            ))}
          </div>
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
        description: `${name} took a shine to ${targetName(data.target_id)}.`,
      };
    case 'became_best_friends':
      return {
        icon: '\u{2B50}',
        description: `${name} and ${targetName(data.target_id)} became best friends.`,
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
                 : critter === 'firefly' ? '\u{2728}'
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
    case 'food_group' as EventType: {
      const g = ev as unknown as { count: number; totalAmount: number };
      return {
        icon: '\u{1F36F}',
        description: g.count === 1
          ? `A foraging haul came in (${g.totalAmount.toFixed(0)}u).`
          : `${g.count} foraging hauls brought in ${g.totalAmount.toFixed(0)}u.`,
      };
    }
    case 'play_group' as EventType: {
      const count = (ev as unknown as { count: number }).count;
      return {
        icon: '\u{1F389}',
        description: count === 1
          ? 'A parade romped through the chamber.'
          : `${count} parades romped through the chamber.`,
      };
    }
    case 'discovery_group' as EventType: {
      const counts = (data.counts || {}) as Record<string, number>;
      const PLURALS: Record<string, string> = {
        butterfly: 'butterflies', worm: 'worms', firefly: 'fireflies', beetle: 'beetles',
      };
      const parts = Object.entries(counts).map(([k, n]) =>
        n === 1 ? `a ${k}` : `${n} ${PLURALS[k] || k + 's'}`);
      const listed = parts.length === 1
        ? parts[0]
        : `${parts.slice(0, -1).join(', ')} and ${parts[parts.length - 1]}`;
      const kinds = Object.keys(counts);
      const icon = kinds.length === 1
        ? (kinds[0] === 'butterfly' ? '\u{1F98B}'
           : kinds[0] === 'worm' ? '\u{1FAB1}'
           : kinds[0] === 'firefly' ? '\u{2728}' : '\u{1FAB2}')
        : '\u{1F50D}';
      return { icon, description: `The colony found ${listed}.` };
    }
    case 'tap_group' as EventType: {
      const count = (ev as unknown as { count: number }).count;
      return {
        icon: '\u{1FAF6}',
        description: count === 1
          ? 'The keeper dropped in a helping of food.'
          : `The keeper dropped in ${count} helpings of food.`,
      };
    }
    case 'flicker_group' as EventType: {
      const count = (ev as unknown as { count: number }).count;
      return {
        icon: '\u{1F6AA}',
        description: count <= 2
          ? 'A neighbour module connected.'
          : `The tunnel to a neighbour flickered ${count} times.`,
      };
    }
    default:
      return {
        icon: '\u{1F4AC}',
        description: `${name}: ${ev.type.replace(/_/g, ' ')}.`,
      };
  }
}
