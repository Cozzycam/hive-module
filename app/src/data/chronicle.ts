// The Annals — the colony's history as its progression system. Deeds are
// story beats the historian left room for; renown is reputation the colony
// accrues; observances are anniversaries that call back to it. Everything
// derives from the event log + snapshot, client-side, in the style of
// composeSaga/buildGallery. No XP, no bars, no streaks.
//
// The recorded-deed register (localStorage, hive_annals_<colonyId>) is the
// honesty fix: the VPS returns only the newest ~1000 events, so a deed, once
// recorded, is written down locally and never recomputed — the ledger is
// history, not status. Counting tallies fold in with an event-unix watermark
// so they survive the window too.
import type { ColonyEvent, ColonySnapshot } from '../api/types';
import { nameFromId } from './plantNames';
import { isAccessory, artKindLabel } from './artworks';
import { traitLabel } from './traits';

const DAY = 86400;

export type DeedArc = 'founding' | 'growing' | 'trials' | 'memory' | 'renown';
export type DeedState = 'recorded' | 'rumoured' | 'unwritten';

export const ARC_ORDER: DeedArc[] = ['founding', 'growing', 'trials', 'memory', 'renown'];
export const ARC_LABELS: Record<DeedArc, string> = {
  founding: 'I — The Founding',
  growing: 'II — The Growing Season',
  trials: 'III — The Trials',
  memory: 'IV — The Long Memory',
  renown: 'V — Renown',
};

export interface Deed {
  id: string;
  arc: DeedArc;
  icon: string;
  title: string;
  state: DeedState;
  hint?: string;          // rumoured: the invitation, in italics
  inscription?: string;   // recorded: one line with real names (no date — render from unix)
  unix?: number;          // recorded
  protagonists?: string[];
  callback?: string;      // seeds the saga's remembered-deeds map
  progress?: number;      // 0..1 where countable (never rendered as a meter)
  gentle?: boolean;       // may appear on Home even from the Long Memory arc
}

export interface Era {
  name: string;
  startUnix: number;
  endUnix?: number;       // absent = present
  summary?: string;       // one sentence, composed from in-window events
}

export interface RenownTier { tier: number; phrase: string; unix: number; }

export interface RenownFamily {
  id: string;
  label: string;
  earned: RenownTier[];   // dated history, oldest first
  standing?: string;      // current phrase, if any tier earned
  nextHint: string;       // prose about the next tier only
}

export interface Observance {
  unix: number;
  dateKey: string;        // YYYY-MM-DD, local
  priority: number;       // 1 remembering, 2 founding, 3 deed anniversary
  line: string;
}

export interface Annals {
  deeds: Deed[];
  eras: Era[];
  families: RenownFamily[];
  title: string | null;         // colony display title — most recent tier phrase
  kindnessCount: number;        // wish_granted, window-safe
  oldestEventUnix: number | null;
  truncated: boolean;           // records start meaningfully after the founding
}

// ---- The register ------------------------------------------------------

interface RegDeed {
  unix: number;
  inscription: string;
  protagonists?: string[];
  callback?: string;
}

interface Register {
  schema: number;
  deeds: Record<string, RegDeed>;
  renown: Record<string, { tier: number; unix: number }[]>;
  eraTurns: Record<string, number>;
  counts: Record<string, { n: number; mark: number }>;
  sets: Record<string, string[]>;
}

export function annalsKey(colonyId: string): string {
  return `hive_annals_${colonyId}`;
}

function freshRegister(): Register {
  return { schema: 1, deeds: {}, renown: {}, eraTurns: {}, counts: {}, sets: {} };
}

function loadRegister(colonyId: string): Register {
  try {
    const raw = localStorage.getItem(annalsKey(colonyId));
    if (!raw) return freshRegister();
    const r = JSON.parse(raw) as Register;
    if (!r || r.schema !== 1) return freshRegister();
    return {
      schema: 1,
      deeds: r.deeds || {},
      renown: r.renown || {},
      eraTurns: r.eraTurns || {},
      counts: r.counts || {},
      sets: r.sets || {},
    };
  } catch { return freshRegister(); }
}

function saveRegister(colonyId: string, reg: Register): void {
  try { localStorage.setItem(annalsKey(colonyId), JSON.stringify(reg)); }
  catch { /* localStorage full — the ledger just recomputes next time */ }
}

// ---- Shared helpers ------------------------------------------------------

function d(ev: ColonyEvent): Record<string, unknown> {
  return (ev.data || {}) as Record<string, unknown>;
}

function evName(ev: ColonyEvent): string {
  return (ev as unknown as { name?: string }).name
    || (ev.lilguy ? nameFromId(ev.lilguy) : 'someone');
}

function targetName(ev: ColonyEvent): string {
  const data = d(ev);
  return (data.target_name as string)
    || (typeof data.target_id === 'number' ? nameFromId(data.target_id) : 'a nestmate');
}

const NUM_WORDS = ['no', 'one', 'two', 'three', 'four', 'five', 'six', 'seven',
  'eight', 'nine', 'ten', 'eleven', 'twelve'];
export function numWord(n: number): string {
  return NUM_WORDS[n] ?? String(n);
}

export function nth(n: number): string {
  const mod100 = n % 100;
  if (mod100 >= 11 && mod100 <= 13) return `${n}th`;
  switch (n % 10) {
    case 1: return `${n}st`;
    case 2: return `${n}nd`;
    case 3: return `${n}rd`;
    default: return `${n}th`;
  }
}

export function annalsDate(unix: number): string {
  return new Date(unix * 1000).toLocaleDateString(undefined, { day: 'numeric', month: 'long' });
}

// Keep in lockstep with CritterKind (firmware chamber.h / data/critters.ts)
const CRITTER_KINDS = ['beetle', 'butterfly', 'worm', 'firefly', 'moth', 'snail', 'ladybird', 'dragonfly'];

const CHALLENGE_LABELS: Record<string, string> = {
  heatwave: 'heatwave', cold_snap: 'cold snap', drought: 'drought', storm: 'storm',
};

// ---- Counter fold --------------------------------------------------------

// Counting tallies survive the 1000-event window: only events newer than the
// watermark increment, and the watermark advances to the newest match.
const COUNTERS: { id: string; match: (ev: ColonyEvent) => boolean }[] = [
  { id: 'discoveries', match: ev => ev.type === 'discovery' },
  { id: 'crops', match: ev => ev.type === 'crop_sown' },
  { id: 'trials', match: ev => ev.type === 'challenge_end' },
  { id: 'kindness', match: ev => ev.type === 'wish_granted' },
  { id: 'friendships', match: ev => ev.type === 'became_best_friends' },
  {
    id: 'works',
    match: ev => ev.type === 'crafted'
      && !isAccessory(String(d(ev).kind)) && String(d(ev).kind) !== 'memorial',
  },
  { id: 'memorials', match: ev => ev.type === 'crafted' && String(d(ev).kind) === 'memorial' },
  { id: 'anniversaries', match: ev => ev.type === 'mourning' && !!d(ev).anniversary },
];

interface Fold {
  events: ColonyEvent[];              // ascending, full window
  snapshot: ColonySnapshot | null;
  reg: Register;
  nowUnix: number;
  truncated: boolean;                 // window starts meaningfully after founding
  crossUnix: Map<string, number>;     // `${counter}:${n}` → unix of the Nth event
  lastMatch: Map<string, number>;     // counter → newest matching unix this fold
  dirty: boolean;
}

function count(f: Fold, id: string): number {
  return f.reg.counts[id]?.n ?? 0;
}

function setOf(f: Fold, id: string): Set<string> {
  return new Set(f.reg.sets[id] || []);
}

function foldCountersAndSets(f: Fold): void {
  for (const c of COUNTERS) {
    const cur = f.reg.counts[c.id] || { n: 0, mark: 0 };
    let { n, mark } = cur;
    for (const ev of f.events) {
      if (ev.unix <= cur.mark || !c.match(ev)) continue;
      n++;
      f.crossUnix.set(`${c.id}:${n}`, ev.unix);
      f.lastMatch.set(c.id, ev.unix);
      if (ev.unix > mark) mark = ev.unix;
    }
    if (n !== cur.n || mark !== cur.mark) {
      f.reg.counts[c.id] = { n, mark };
      f.dirty = true;
    }
  }

  // Sets are idempotent unions — no watermark needed.
  const unions: { id: string; values: string[] }[] = [
    {
      id: 'critters',
      values: f.events.filter(e => e.type === 'discovery')
        .map(e => String(d(e).critter || '')).filter(Boolean),
    },
    {
      id: 'catchers',   // distinct-holder dedupe guards the pre-v179 crown-thrash archives
      values: f.events.filter(e => e.type === 'trait_earned' && String(d(e).trait) === 'catcher')
        .map(e => String(e.lilguy)),
    },
    {
      id: 'survived',   // colony-wide trial kinds, backfilled from the living roster
      values: [
        ...f.events.filter(e => e.type === 'trait_earned'
          && String(d(e).trait).startsWith('survived_')).map(e => String(d(e).trait)),
        ...(f.snapshot?.lilguys || []).flatMap(l =>
          (l.traits || []).filter(t => t.startsWith('survived_'))),
      ],
    },
  ];
  for (const u of unions) {
    const s = setOf(f, u.id);
    const before = s.size;
    for (const v of u.values) s.add(v);
    if (s.size !== before) {
      f.reg.sets[u.id] = [...s].sort();
      f.dirty = true;
    }
  }
}

// ---- The deed table ------------------------------------------------------

interface DeedDef {
  id: string;
  arc: DeedArc;
  icon: string;
  title: string;
  gentle?: boolean;
  reveal?: (f: Fold) => boolean;      // extra gate on the rumoured state only
  hint: (f: Fold) => string;
  progress?: (f: Fold) => number | undefined;
  find: (f: Fold) => RegDeed | null;
}

function firstEv(f: Fold, pred: (ev: ColonyEvent) => boolean): ColonyEvent | undefined {
  return f.events.find(pred);
}

// Honesty framing: when the window starts after the founding on a colony
// that has lived (and lost) before the chronicle began, "firsts" are only
// the first the chronicle remembers — never falsified as absolute firsts.
function firstPhrase(f: Fold, thing: string): string {
  return f.truncated
    ? `the first ${thing} the chronicle remembers`
    : `the colony's first ${thing}`;
}

function rosterName(f: Fold, id: number): string {
  return f.snapshot?.lilguys?.find(l => l.id === id)?.name || nameFromId(id);
}

// Number alive right now — the honest census when history has scrolled away.
function aliveNow(f: Fold): number {
  return f.snapshot?.population?.alive ?? 0;
}

const DEED_DEFS: DeedDef[] = [
  // ---- Arc I — The Founding (all attainable in the first days) ----
  {
    id: 'first-hatching', arc: 'founding', icon: '\u{1F423}', title: 'The First Hatching',
    hint: () => 'The founding eggs lie in the warm dark, waiting.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'hatch');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} hatched, ${firstPhrase(f, 'of them all')}`,
        protagonists: [name], callback: 'the first to hatch',
      };
    },
  },
  {
    id: 'founders-assembled', arc: 'founding', icon: '\u{1F331}', title: 'The Founders Assembled',
    hint: () => 'Not all the founders have opened their eyes.',
    progress: f => {
      const cohort = f.snapshot?.population?.by_role?.founder || 0;
      if (!cohort) return undefined;
      const n = f.events.filter(e => e.type === 'hatch' && !!d(e).is_pioneer).length;
      return Math.min(1, n / cohort);
    },
    find: f => {
      const cohort = f.snapshot?.population?.by_role?.founder || 0;
      if (!cohort) return null;
      const pioneers = f.events.filter(e => e.type === 'hatch' && !!d(e).is_pioneer);
      if (pioneers.length < cohort) return null;
      const last = pioneers[cohort - 1];
      return {
        unix: last.unix,
        inscription: `${evName(last)} opened their eyes, and the ${numWord(cohort)} founders stood together`,
        protagonists: pioneers.slice(0, cohort).map(evName),
      };
    },
  },
  {
    id: 'first-shine', arc: 'founding', icon: '\u{1F91D}', title: 'First Shine',
    hint: () => 'No one has yet taken a shine to anyone. Give them time.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'bond_formed');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} took ${firstPhrase(f, 'shine')} — to ${targetName(ev)}`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'first-haul', arc: 'founding', icon: '\u{1F35E}', title: 'The First Haul',
    hint: () => 'The larder waits for its first forager.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'food_delivered');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} carried home ${firstPhrase(f, 'haul')}`,
        protagonists: [name], callback: 'who carried home the colony’s first haul',
      };
    },
  },
  {
    id: 'keeper-provides', arc: 'founding', icon: '\u{1F381}', title: 'The Keeper Provides',
    hint: () => 'The keeper has not yet fed the colony by hand.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'food_tap');
      if (!ev) return null;
      return { unix: ev.unix, inscription: 'the keeper fed the colony by hand for the first time' };
    },
  },
  {
    id: 'new-cache', arc: 'founding', icon: '\u{1F50E}', title: 'A New Cache',
    hint: () => 'Somewhere out there is food no one has found.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'food_discovered');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} found food no one had known about`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'first-visitor', arc: 'founding', icon: '\u{1F98B}', title: 'The First Visitor',
    hint: () => 'The Field Guide is a book of empty pages.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'discovery');
      if (!ev) return null;
      const name = evName(ev);
      const critter = String(d(ev).critter || 'visitor');
      return {
        unix: ev.unix,
        inscription: `${name} found ${firstPhrase(f, critter)}`,
        protagonists: [name],
        callback: `who found the colony’s first ${critter}`,
      };
    },
  },
  {
    id: 'first-parade', arc: 'founding', icon: '\u{1F389}', title: 'The First Parade',
    hint: () => 'No parade has yet romped through the chamber.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'play' && String(d(e).kind || 'parade') === 'parade');
      if (!ev) return null;
      const name = evName(ev);
      const n = Number(d(ev).participants) || 0;
      return {
        unix: ev.unix,
        inscription: `${name} led ${firstPhrase(f, 'parade')}${n > 1 ? `, ${numWord(n)} strong` : ''}`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'garden-wakes', arc: 'founding', icon: '\u{1F33E}', title: 'The Garden Wakes',
    hint: () => 'No seed has yet been pressed into the earth.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'crop_sown');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} pressed ${firstPhrase(f, 'seed')} into the earth`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'wish-answered', arc: 'founding', icon: '\u{1F4AD}', title: 'A Wish, Answered',
    hint: () => 'When a wish rises, the keeper may answer it.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'wish_granted');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} wished, and the keeper answered`,
        protagonists: [name],
      };
    },
  },

  // ---- Arc II — The Growing Season ----
  {
    id: 'best-friends', arc: 'growing', icon: '\u{2B50}', title: 'Best Friends',
    hint: f => {
      const shines = f.events.filter(e => e.type === 'bond_formed').length;
      return shines > 0
        ? 'Shines have been taken, but none yet returned.'
        : 'Friendship starts with a shine no one has yet taken.';
    },
    find: f => {
      const ev = firstEv(f, e => e.type === 'became_best_friends');
      if (!ev) return null;
      const a = evName(ev);
      const b = targetName(ev);
      return {
        unix: ev.unix,
        inscription: `${a} and ${b} became ${firstPhrase(f, 'true friends')}`,
        protagonists: [a, b],
      };
    },
  },
  {
    id: 'name-earned', arc: 'growing', icon: '\u{1F3C5}', title: 'A Name Earned',
    hint: () => 'No one has yet done a thing the colony gives names for.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'trait_earned');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} earned “${traitLabel(String(d(ev).trait || ''))}” — the first name given`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'muse-arrives', arc: 'growing', icon: '\u{1F3A8}', title: 'The Muse Arrives',
    hint: () => 'The chamber walls are bare. The muse has not yet visited.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'crafted'
        && !isAccessory(String(d(e).kind)) && String(d(e).kind) !== 'memorial');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `the muse visited ${name}, who left a ${artKindLabel(String(d(ev).kind || 'work'))} in the chamber`,
        protagonists: [name],
        callback: 'whom the muse visited first',
      };
    },
  },
  {
    id: 'gift-given', arc: 'growing', icon: '\u{1F49D}', title: 'A Gift Given',
    hint: () => 'No petal hat, no pendant, no woven band — not yet.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'crafted' && isAccessory(String(d(e).kind)));
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} made a ${artKindLabel(String(d(ev).kind || 'gift'))} for ${String(d(ev).honoree || 'a friend')}`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'beyond-first-chamber', arc: 'growing', icon: '\u{1F573}\u{FE0F}', title: 'Beyond the First Chamber',
    reveal: f => (f.snapshot?.modules?.length || 0) > 1,
    hint: () => 'A tunnel stands open that no one has dared.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'chamber_crossing');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} crossed into a new chamber, first of them all`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'guide-half', arc: 'growing', icon: '\u{1F4D2}', title: 'The Guide Half-Full',
    hint: f => {
      const n = setOf(f, 'critters').size;
      return `${n === 0 ? 'No' : numWord(n).charAt(0).toUpperCase() + numWord(n).slice(1)} kind${n === 1 ? '' : 's'} of visitor ha${n === 1 ? 's' : 've'} signed the Guide; the rest remain strangers.`;
    },
    progress: f => Math.min(1, setOf(f, 'critters').size / 4),
    find: f => nthDistinctCritter(f, 4),
  },
  {
    id: 'guide-complete', arc: 'growing', icon: '\u{1F4D5}', title: 'The Complete Guide',
    reveal: f => !!f.reg.deeds['guide-half'],
    hint: () => 'The Guide still has silhouettes.',
    progress: f => Math.min(1, setOf(f, 'critters').size / CRITTER_KINDS.length),
    find: f => nthDistinctCritter(f, CRITTER_KINDS.length),
  },
  {
    id: 'ten-strong', arc: 'growing', icon: '\u{1F41C}', title: 'Ten Strong',
    hint: f => `The colony is ${numWord(aliveNow(f))} strong; the chronicler rules a page for ten.`,
    progress: f => Math.min(1, aliveNow(f) / 10),
    find: f => {
      // Prefer the firmware milestone for dating; the snapshot census is the
      // honest fallback once early hatches have left the window.
      const ms = firstEv(f, e => e.type === 'milestone'
        && String(d(e).kind) === 'workers_born' && Number(d(e).value) >= 10);
      if (ms) return { unix: ms.unix, inscription: 'the colony numbered ten — a page ruled for a crowd' };
      if (aliveNow(f) >= 10) {
        return { unix: f.nowUnix, inscription: 'the chronicle counted ten alive under one roof' };
      }
      return null;
    },
  },
  {
    id: 'raised-by-hand', arc: 'growing', icon: '\u{1F9B7}', title: 'Raised by Hand',
    hint: () => 'No hatchling has yet been taken under a wing.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'tended_by_assigned' && typeof d(e).carer_id === 'number');
      if (!ev) return null;
      const carer = rosterName(f, Number(d(ev).carer_id));
      const child = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${carer} took ${child} under a wing`,
        protagonists: [carer, child],
      };
    },
  },
  {
    id: 'third-generation', arc: 'growing', icon: '\u{1F33F}', title: 'The Third Generation',
    reveal: f => !!f.reg.deeds['raised-by-hand'],
    hint: () => 'Someday one who was raised will do the raising.',
    find: f => {
      const tendeds = f.events.filter(e => e.type === 'tended_by_assigned');
      const raised = new Set<number>();
      for (const ev of tendeds) {
        const carer = Number(d(ev).carer_id);
        if (raised.has(carer)) {
          const carerName = rosterName(f, carer);
          return {
            unix: ev.unix,
            inscription: `${carerName}, once raised by hand, took ${evName(ev)} under a wing in turn`,
            protagonists: [carerName, evName(ev)],
          };
        }
        if (ev.lilguy) raised.add(ev.lilguy);
      }
      return null;
    },
  },

  // ---- Arc III — The Trials (arc unwritten until weather first comes) ----
  {
    id: 'colony-held', arc: 'trials', icon: '\u{26C8}\u{FE0F}', title: 'The Colony Held',
    hint: f => {
      const active = f.snapshot?.world?.active_challenges?.[0];
      return active
        ? `A ${CHALLENGE_LABELS[active.type] || 'trial'} grips the colony even now.`
        : 'Weather has come before; the colony has yet to see one out.';
    },
    find: f => {
      const ev = firstEv(f, e => e.type === 'challenge_end');
      if (!ev) return null;
      const kind = CHALLENGE_LABELS[String(d(ev).type)] || 'trial';
      return { unix: ev.unix, inscription: `the ${kind} passed, and the colony still stood` };
    },
  },
  ...([
    { id: 'storm-tested', trait: 'survived_storm', title: 'Storm-Tested', noun: 'storm', hint: 'No storm has yet been outlasted.' },
    { id: 'sun-scorched', trait: 'survived_heatwave', title: 'Sun-Scorched', noun: 'heatwave', hint: 'No heatwave has yet been outlasted.' },
    { id: 'frost-hardy', trait: 'survived_cold_snap', title: 'Frost-Hardy', noun: 'cold snap', hint: 'No cold snap has yet been outlasted.' },
    { id: 'dust-weathered', trait: 'survived_drought', title: 'Dust-Weathered', noun: 'drought', hint: 'No drought has yet been outlasted.' },
  ].map((s): DeedDef => ({
    id: s.id, arc: 'trials', icon: '\u{1F6E1}\u{FE0F}', title: s.title,
    hint: () => s.hint,
    find: f => {
      const ev = firstEv(f, e => e.type === 'trait_earned' && String(d(e).trait) === s.trait);
      if (ev) {
        const name = evName(ev);
        return {
          unix: ev.unix,
          inscription: `${name} came through the ${s.noun} — and will not forget`,
          protagonists: [name],
        };
      }
      // Roster backfill: the trial predates the chronicle's window
      const survivor = f.snapshot?.lilguys?.find(l => (l.traits || []).includes(s.trait));
      if (survivor) {
        return {
          unix: f.nowUnix,
          inscription: `the chronicle finds ${survivor.name} bearing the mark of a ${s.noun} outlasted`,
          protagonists: [survivor.name],
        };
      }
      return null;
    },
  }))),
  {
    id: 'all-four-trials', arc: 'trials', icon: '\u{1F3D4}\u{FE0F}', title: 'All Four Trials',
    hint: f => {
      const known = setOf(f, 'survived');
      const names = ['survived_heatwave', 'survived_cold_snap', 'survived_drought', 'survived_storm'];
      const met = names.filter(t => known.has(t)).map(t => CHALLENGE_LABELS[t.slice(9)] || t);
      const left = names.filter(t => !known.has(t)).map(t => CHALLENGE_LABELS[t.slice(9)] || t);
      if (met.length === 0) return 'Heat, cold, drought and storm all wait their turn.';
      return `The colony has known ${listJoin(met)}; ${listJoin(left)} it has yet to meet.`;
    },
    progress: f => Math.min(1, setOf(f, 'survived').size / 4),
    find: f => {
      if (setOf(f, 'survived').size < 4) return null;
      const last = [...f.events].reverse().find(e => e.type === 'trait_earned'
        && String(d(e).trait).startsWith('survived_'));
      return {
        unix: last?.unix ?? f.nowUnix,
        inscription: 'heat, cold, drought and storm — all four trials endured',
      };
    },
  },
  {
    id: 'one-unbreakable', arc: 'trials', icon: '\u{1FAA8}', title: 'One Unbreakable',
    reveal: f => setOf(f, 'survived').size >= 2,
    hint: () => 'No one has yet stood through two kinds of weather.',
    find: f => {
      // From events: the second distinct survived_* trait on one conker
      const byGuy = new Map<number, Set<string>>();
      for (const ev of f.events) {
        if (ev.type !== 'trait_earned' || !String(d(ev).trait).startsWith('survived_')) continue;
        const s = byGuy.get(ev.lilguy) || new Set<string>();
        s.add(String(d(ev).trait));
        byGuy.set(ev.lilguy, s);
        if (s.size >= 2) {
          const name = evName(ev);
          return {
            unix: ev.unix,
            inscription: `${name} survived their second trial — the Unbreakable walks among us`,
            protagonists: [name],
          };
        }
      }
      // Roster backfill (matches the epithet in biography.ts)
      const tough = f.snapshot?.lilguys?.find(l =>
        (l.traits || []).filter(t => t.startsWith('survived_')).length >= 2);
      if (tough) {
        return {
          unix: f.nowUnix,
          inscription: `the chronicle finds ${tough.name} twice-tried and unbroken`,
          protagonists: [tough.name],
        };
      }
      return null;
    },
  },

  // ---- Arc IV — The Long Memory (unwritten until the first death;
  // never hinted beforehand, never a goal, never toasted) ----
  {
    id: 'first-loss', arc: 'memory', icon: '\u{1F342}', title: 'The First Loss',
    hint: () => 'The colony has known loss the chronicle did not witness.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'death');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} was ${firstPhrase(f, 'loss')}`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'first-vigil', arc: 'memory', icon: '\u{1F56F}\u{FE0F}', title: 'The First Vigil',
    hint: () => 'None have yet stood vigil.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'mourning' && !d(e).anniversary);
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} stood vigil for ${String(d(ev).dead_name || 'the fallen')}`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'stone-raised', arc: 'memory', icon: '\u{1FAA6}', title: 'A Stone Raised',
    hint: f => {
      const death = firstEv(f, e => e.type === 'death');
      return `No stone yet stands for ${death ? evName(death) : 'the fallen'}.`;
    },
    find: f => {
      const ev = firstEv(f, e => e.type === 'crafted' && String(d(e).kind) === 'memorial');
      if (!ev) return null;
      const maker = evName(ev);
      const honoree = String(d(ev).honoree || 'the fallen');
      return {
        unix: ev.unix,
        inscription: `${maker} carved a stone for ${honoree}`,
        protagonists: [maker, honoree],
      };
    },
  },
  {
    id: 'remembering-day', arc: 'memory', icon: '\u{1F490}', title: 'Remembering Day',
    hint: () => 'The stones wait to be visited.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'mourning' && !!d(e).anniversary);
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} walked back to ${String(d(ev).dead_name || 'an old friend')}’s stone, remembering`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'long-remembered', arc: 'memory', icon: '\u{1F4FF}', title: 'Long Remembered',
    hint: () => 'Memory is long here — how long, time will tell.',
    find: f => {
      const deathBy = new Map<number, ColonyEvent>();
      for (const ev of f.events) {
        if (ev.type === 'death' && !deathBy.has(ev.lilguy)) deathBy.set(ev.lilguy, ev);
      }
      for (const ev of f.events) {
        if (ev.type !== 'mourning' || !d(ev).anniversary) continue;
        const death = deathBy.get(Number(d(ev).dead_id));
        if (death && ev.unix - death.unix >= 28 * DAY) {
          const name = evName(ev);
          const days = Math.floor((ev.unix - death.unix) / DAY);
          return {
            unix: ev.unix,
            inscription: `${name} still walks to ${String(d(ev).dead_name || evName(death))}’s stone, ${days} days on`,
            protagonists: [name],
          };
        }
      }
      return null;
    },
  },
  {
    id: 'peaceful-end', arc: 'memory', icon: '\u{1F54A}\u{FE0F}', title: 'A Peaceful End',
    hint: () => 'May every ending have a friend beside it.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'death'
        && String(d(e).cause) !== 'starvation' && !!d(e).attended_by);
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} passed peacefully, ${String(d(ev).attended_by)} at their side`,
        protagonists: [name],
      };
    },
  },
  {
    id: 'elder-among-us', arc: 'memory', icon: '\u{1F9D3}', title: 'An Elder Among Us',
    gentle: true,   // lives in the Long Memory but is gentle enough for Home
    hint: () => 'No one has yet grown old here.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'trait_earned' && String(d(e).trait) === 'elder');
      if (!ev) return null;
      const name = evName(ev);
      return {
        unix: ev.unix,
        inscription: `${name} grew old — the colony’s first elder`,
        protagonists: [name],
      };
    },
  },

  // ---- Arc V — Renown deeds (the long run) ----
  {
    id: 'hundredth-visitor', arc: 'renown', icon: '\u{1F4AF}', title: 'The Hundredth Visitor',
    hint: f => {
      const n = count(f, 'discoveries');
      return `The Guide records ${n} sighting${n === 1 ? '' : 's'}; the chronicler awaits the hundredth.`;
    },
    progress: f => Math.min(1, count(f, 'discoveries') / 100),
    find: f => {
      if (count(f, 'discoveries') < 100) return null;
      const unix = f.crossUnix.get('discoveries:100') ?? f.lastMatch.get('discoveries') ?? f.nowUnix;
      return { unix, inscription: 'the hundredth visitor was written into the Guide' };
    },
  },
  {
    id: 'gallery', arc: 'renown', icon: '\u{1F3DB}\u{FE0F}', title: 'A Gallery',
    hint: f => {
      const n = standingWorksNow(f.events);
      return `${n === 0 ? 'No' : numWord(n).charAt(0).toUpperCase() + numWord(n).slice(1)} work${n === 1 ? '' : 's'} stand${n === 1 ? 's' : ''} in the chamber.`;
    },
    progress: f => Math.min(1, standingWorksNow(f.events) / 5),
    find: f => {
      // Exact buildGallery reconstruction: crafted adds a work; art_weathered
      // retires the oldest standing work matching maker + kind.
      const works: { maker: string; kind: string; weathered: boolean }[] = [];
      let standing = 0;
      for (const ev of f.events) {
        if (ev.type === 'crafted') {
          works.push({ maker: evName(ev), kind: String(d(ev).kind || 'work'), weathered: false });
          standing++;
          if (standing >= 5) {
            return { unix: ev.unix, inscription: 'five works stood in the chamber at once — a gallery' };
          }
        } else if (ev.type === 'art_weathered') {
          const victim = works.find(w =>
            !w.weathered && w.maker === evName(ev) && w.kind === String(d(ev).kind || ''));
          if (victim) { victim.weathered = true; standing--; }
        }
      }
      return null;
    },
  },
  {
    id: 'weathered-not-lost', arc: 'renown', icon: '\u{1F32B}\u{FE0F}', title: 'Weathered, Not Lost',
    reveal: f => !!f.reg.deeds['muse-arrives'],
    hint: () => 'Nothing made here has yet faded.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'art_weathered');
      if (!ev) return null;
      return {
        unix: ev.unix,
        inscription: `a ${artKindLabel(String(d(ev).kind || 'work'))} faded back into the earth — nothing made here is truly lost`,
      };
    },
  },
  {
    id: 'title-lineage', arc: 'renown', icon: '\u{1F451}', title: 'A Title With a Lineage',
    hint: f => {
      const n = setOf(f, 'catchers').size;
      return `The Bug Hunter title has known ${n === 0 ? 'no' : numWord(n)} holder${n === 1 ? '' : 's'}.`;
    },
    progress: f => Math.min(1, setOf(f, 'catchers').size / 3),
    find: f => {
      if (setOf(f, 'catchers').size < 3) return null;
      // Walk the window for the event that crowned the third distinct holder
      const seen = new Set<string>();
      for (const ev of f.events) {
        if (ev.type !== 'trait_earned' || String(d(ev).trait) !== 'catcher') continue;
        seen.add(String(ev.lilguy));
        if (seen.size >= 3) {
          const name = evName(ev);
          return {
            unix: ev.unix,
            inscription: `the Bug Hunter title passed to ${name}, its third holder`,
            protagonists: [name],
          };
        }
      }
      return { unix: f.nowUnix, inscription: 'the Bug Hunter title passed to its third holder' };
    },
  },
  {
    id: 'neighbours', arc: 'renown', icon: '\u{1F30D}', title: 'Neighbours',
    hint: () => 'No word has yet come from beyond the border.',
    find: f => {
      const ev = firstEv(f, e => e.type === 'colony_event'
        && String(d(e).kind) === 'care_package_from_neighbours');
      if (!ev) return null;
      return { unix: ev.unix, inscription: 'word came from beyond the border — a care package from neighbours' };
    },
  },
  {
    id: 'hundred-days', arc: 'renown', icon: '\u{1F4C6}', title: 'A Hundred Days',
    hint: f => {
      const days = colonyAgeDays(f);
      return `The colony is ${days} day${days === 1 ? '' : 's'} old.`;
    },
    progress: f => Math.min(1, colonyAgeDays(f) / 100),
    find: f => {
      const founded = f.snapshot?.founded_unix || 0;
      if (!founded || f.nowUnix - founded < 100 * DAY) return null;
      return { unix: founded + 100 * DAY, inscription: 'one hundred days since the founding' };
    },
  },
  {
    id: 'year-of-days', arc: 'renown', icon: '\u{1F324}\u{FE0F}', title: 'One Year of Days',
    reveal: f => !!f.reg.deeds['hundred-days'],
    hint: f => {
      const days = colonyAgeDays(f);
      return `The colony is ${days} days old; a year is a long story.`;
    },
    progress: f => Math.min(1, colonyAgeDays(f) / 365),
    find: f => {
      const founded = f.snapshot?.founded_unix || 0;
      if (!founded || f.nowUnix - founded < 365 * DAY) return null;
      return { unix: founded + 365 * DAY, inscription: 'a full year of days, written down' };
    },
  },
];

function listJoin(items: string[]): string {
  if (items.length === 0) return 'nothing';
  if (items.length === 1) return items[0];
  return `${items.slice(0, -1).join(', ')} and ${items[items.length - 1]}`;
}

function colonyAgeDays(f: Fold): number {
  const founded = f.snapshot?.founded_unix || 0;
  if (!founded) return 0;
  return Math.max(0, Math.floor((f.nowUnix - founded) / DAY));
}

// The discovery event at which the window's cumulative distinct critter
// kinds reached n. Falls back to the newest discovery if the earlier kinds
// predate the window (the persisted set knows more than the window shows).
function nthDistinctCritter(f: Fold, n: number): RegDeed | null {
  if (setOf(f, 'critters').size < n) return null;
  const seen = new Set<string>();
  for (const ev of f.events) {
    if (ev.type !== 'discovery') continue;
    const kind = String(d(ev).critter || '');
    if (!kind || seen.has(kind)) continue;
    seen.add(kind);
    if (seen.size >= n) {
      const name = evName(ev);
      const complete = n >= CRITTER_KINDS.length;
      return {
        unix: ev.unix,
        inscription: complete
          ? `${name} found a ${kind}, and the Guide was complete — every visitor known`
          : `${name} found a ${kind} — the ${nth(n)} kind of visitor in the Guide`,
        protagonists: [name],
      };
    }
  }
  const last = f.lastMatch.get('discoveries');
  return last
    ? { unix: last, inscription: `the Guide’s ${nth(n)} kind of visitor signed its pages` }
    : null;
}

function standingWorksNow(events: ColonyEvent[]): number {
  const works: { maker: string; kind: string; weathered: boolean }[] = [];
  for (const ev of events) {
    if (ev.type === 'crafted') {
      works.push({ maker: evName(ev), kind: String(d(ev).kind || 'work'), weathered: false });
    } else if (ev.type === 'art_weathered') {
      const victim = works.find(w =>
        !w.weathered && w.maker === evName(ev) && w.kind === String(d(ev).kind || ''));
      if (victim) victim.weathered = true;
    }
  }
  return works.filter(w => !w.weathered).length;
}

// ---- Arc gating -----------------------------------------------------------

function arcRevealed(arc: DeedArc, f: Fold): boolean {
  if (arc === 'trials') {
    return f.events.some(e => e.type === 'challenge_start' || e.type === 'challenge_end')
      || (f.snapshot?.world?.active_challenges?.length || 0) > 0
      || setOf(f, 'survived').size > 0
      || DEED_DEFS.some(def => def.arc === 'trials' && !!f.reg.deeds[def.id]);
  }
  if (arc === 'memory') {
    return f.events.some(e => e.type === 'death')
      || (f.snapshot?.population?.dead_total || 0) > 0
      || DEED_DEFS.some(def => def.arc === 'memory' && !!f.reg.deeds[def.id]);
  }
  return true;
}

// ---- Renown ---------------------------------------------------------------

interface FamilyDef {
  id: string;
  label: string;
  tiers: { phrase: string }[];
  // returns per-tier: earned? + unix when it crossed (this fold or fallback)
  earnedTier: (f: Fold, tier: number) => number | null;
  nextHint: (f: Fold, nextTier: number, colonyName: string) => string;
}

function counterFamily(
  id: string, label: string, counter: string, noun: [string, string],
  tiers: { count: number; phrase: string }[],
): FamilyDef {
  return {
    id, label,
    tiers: tiers.map(t => ({ phrase: t.phrase })),
    earnedTier: (f, tier) => {
      const t = tiers[tier - 1];
      if (count(f, counter) < t.count) return null;
      return f.crossUnix.get(`${counter}:${t.count}`)
        ?? f.lastMatch.get(counter) ?? f.nowUnix;
    },
    nextHint: (f, nextTier, colonyName) => {
      const t = tiers[nextTier - 1];
      const need = Math.max(1, t.count - count(f, counter));
      const w = numWord(need);
      return `${w.charAt(0).toUpperCase()}${w.slice(1)} more ${need === 1 ? noun[0] : noun[1]} and ${colonyName} will be “${t.phrase}”.`;
    },
  };
}

const FAMILY_DEFS: FamilyDef[] = [
  counterFamily('makers', 'Makers', 'works', ['work from the muse', 'works from the muse'], [
    { count: 3, phrase: 'where the muse visits' },
    { count: 10, phrase: 'a colony of makers' },
    { count: 25, phrase: 'the muse lives here' },
  ]),
  counterFamily('gardeners', 'Gardeners', 'crops', ['sowing', 'sowings'], [
    { count: 10, phrase: 'green-handed' },
    { count: 50, phrase: 'a garden colony' },
    { count: 200, phrase: 'where the earth answers' },
  ]),
  {
    id: 'rememberers', label: 'Rememberers',
    tiers: [
      { phrase: 'a colony that remembers' },
      { phrase: 'long-memoried' },
      { phrase: 'where no one is forgotten' },
    ],
    earnedTier: (f, tier) => {
      if (tier === 1) {
        if (count(f, 'memorials') < 1 || count(f, 'anniversaries') < 1) return null;
        const a = f.crossUnix.get('memorials:1') ?? f.lastMatch.get('memorials') ?? f.nowUnix;
        const b = f.crossUnix.get('anniversaries:1') ?? f.lastMatch.get('anniversaries') ?? f.nowUnix;
        return Math.max(a, b);
      }
      if (tier === 2) {
        if (count(f, 'anniversaries') < 5) return null;
        return f.crossUnix.get('anniversaries:5') ?? f.lastMatch.get('anniversaries') ?? f.nowUnix;
      }
      const deed = f.reg.deeds['long-remembered'];
      return deed ? deed.unix : null;
    },
    nextHint: (f, nextTier, colonyName) => {
      if (nextTier === 1) {
        return `A stone raised and a remembering walk, and ${colonyName} will be “a colony that remembers”.`;
      }
      if (nextTier === 2) {
        const need = Math.max(1, 5 - count(f, 'anniversaries'));
        const w = numWord(need);
        return `${w.charAt(0).toUpperCase()}${w.slice(1)} more remembering walk${need === 1 ? '' : 's'} and ${colonyName} will be “long-memoried”.`;
      }
      return `When memory proves itself long, ${colonyName} will be “where no one is forgotten”.`;
    },
  },
  counterFamily('weatherworn', 'Weatherworn', 'trials', ['trial weathered', 'trials weathered'], [
    { count: 1, phrase: 'tested once' },
    { count: 4, phrase: 'weatherworn' },
    { count: 10, phrase: 'unbowed by seasons' },
  ]),
  {
    id: 'naturalists', label: 'Naturalists',
    tiers: [
      { phrase: 'curious-eyed' },
      { phrase: 'friends to small things' },
      { phrase: 'keepers of the complete Guide' },
    ],
    earnedTier: (f, tier) => {
      const need = [3, 6, 8][tier - 1];
      if (setOf(f, 'critters').size < need) return null;
      const hit = nthDistinctCritter(f, need);
      return hit ? hit.unix : f.nowUnix;
    },
    nextHint: (f, nextTier, colonyName) => {
      const need = Math.max(1, [3, 6, 8][nextTier - 1] - setOf(f, 'critters').size);
      const w = numWord(need);
      return `${w.charAt(0).toUpperCase()}${w.slice(1)} more kind${need === 1 ? '' : 's'} of visitor and ${colonyName} will be “${FAMILY_DEFS_PHRASE('naturalists', nextTier)}”.`;
    },
  },
  counterFamily('well-kept', 'Well-Kept', 'kindness', ['kindness answered', 'kindnesses answered'], [
    { count: 1, phrase: 'wished-upon and answered' },
    { count: 5, phrase: 'well-kept' },
    { count: 20, phrase: 'beloved of its keeper' },
  ]),
  counterFamily('companions', 'Companions', 'friendships', ['true friendship', 'true friendships'], [
    { count: 1, phrase: 'where friendship took root' },
    { count: 5, phrase: 'a colony of friends' },
    { count: 15, phrase: 'bound tight as roots' },
  ]),
];

function FAMILY_DEFS_PHRASE(id: string, tier: number): string {
  const fam = FAMILY_DEFS.find(fd => fd.id === id);
  return fam?.tiers[tier - 1]?.phrase || '';
}

// ---- Eras ------------------------------------------------------------------

// Deed-based turnings, in the order history offers them. Year turns are
// computed straight from founded_unix.
const ERA_TURNS: { key: string; deed?: string; name: (reg: Register) => string }[] = [
  {
    key: 'first-loss', deed: 'first-loss',
    name: reg => `The Days After ${reg.deeds['first-loss']?.protagonists?.[0] || 'the Loss'}`,
  },
  { key: 'colony-held', deed: 'colony-held', name: () => 'The Weatherworn Days' },
  { key: 'colony-doubled', name: () => 'The Crowded Days' },
  { key: 'neighbours', deed: 'neighbours', name: () => 'The Days of Neighbours' },
];

function foldEraTurns(f: Fold): void {
  for (const t of ERA_TURNS) {
    if (f.reg.eraTurns[t.key] !== undefined) continue;
    if (t.deed) {
      const deed = f.reg.deeds[t.deed];
      if (deed) { f.reg.eraTurns[t.key] = deed.unix; f.dirty = true; }
    } else if (t.key === 'colony-doubled') {
      const cohort = f.snapshot?.population?.by_role?.founder || 0;
      if (cohort > 0 && aliveNow(f) >= cohort * 2) {
        f.reg.eraTurns[t.key] = f.nowUnix;   // dated at first observation — the census is live
        f.dirty = true;
      }
    }
  }
}

function computeEras(f: Fold, foundedUnix: number): Era[] {
  const eras: Era[] = [{ name: 'The Founding Days', startUnix: foundedUnix }];
  const turns: { unix: number; name: string }[] = [];
  for (const t of ERA_TURNS) {
    const unix = f.reg.eraTurns[t.key];
    if (unix !== undefined) turns.push({ unix, name: t.name(f.reg) });
  }
  // Year turns from the clock: "The Second Year", then onward
  const YEAR_NAMES = ['Second', 'Third', 'Fourth', 'Fifth', 'Sixth', 'Seventh'];
  for (let y = 1; foundedUnix + y * 365 * DAY <= f.nowUnix; y++) {
    turns.push({
      unix: foundedUnix + y * 365 * DAY,
      name: `The ${YEAR_NAMES[y - 1] || `${nth(y + 1)}`} Year`,
    });
  }
  turns.sort((a, b) => a.unix - b.unix);

  for (const t of turns) {
    const current = eras[eras.length - 1];
    // Anti-thrash: a turning within a week of the last doesn't open an era
    if (t.unix - current.startUnix < 7 * DAY) continue;
    if (t.unix < current.startUnix) continue;
    current.endUnix = t.unix;
    eras.push({ name: t.name, startUnix: t.unix });
  }

  // Era summaries from in-window events, in the saga's aggregation vocabulary
  for (const era of eras) {
    const end = era.endUnix ?? Infinity;
    const within = f.events.filter(e => e.unix >= era.startUnix && e.unix < end);
    era.summary = composeEraSummary(within);
  }
  return eras;
}

function composeEraSummary(events: ColonyEvent[]): string | undefined {
  const n = (t: string, pred?: (ev: ColonyEvent) => boolean) =>
    events.filter(e => e.type === t && (!pred || pred(e))).length;
  const deaths = n('death');
  const hatches = n('hatch');
  const crops = n('crop_sown');
  const works = n('crafted', e => !isAccessory(String(d(e).kind)) && String(d(e).kind) !== 'memorial');
  const bffs = n('became_best_friends');
  const trials = n('challenge_end');

  const clauses: string[] = [];
  if (deaths > 0) clauses.push(deaths === 1 ? 'one was lost' : `${numWord(deaths)} were lost`);
  if (hatches > 0) clauses.push(hatches === 1 ? 'one hatched' : `${numWord(hatches)} hatched`);
  if (crops > 0) clauses.push(`the garden gave ${crops === 1 ? 'one crop' : `${crops < 13 ? numWord(crops) : crops} crops`}`);
  if (works > 0) clauses.push(`the muse visited ${works === 1 ? 'once' : works === 2 ? 'twice' : `${works} times`}`);
  if (bffs > 0) clauses.push(bffs === 1 ? 'a friendship sealed itself' : `${numWord(bffs)} friendships sealed themselves`);
  if (trials > 0) clauses.push(trials === 1 ? 'a trial was endured' : `${numWord(trials)} trials were endured`);
  if (clauses.length === 0) return undefined;
  const line = clauses.slice(0, 3).join('; ');
  return line.charAt(0).toUpperCase() + line.slice(1) + '.';
}

// ---- The fold --------------------------------------------------------------

/** Load the register, fold the current event window into it (recording any
 *  new deeds, tiers and era turns), persist, and return the full view.
 *  Idempotent — every screen can call it on its own window. */
export function loadAnnals(
  colonyId: string,
  events: ColonyEvent[],
  snapshot: ColonySnapshot | null,
): Annals {
  const reg = loadRegister(colonyId);
  const sorted = [...events].sort((a, b) => a.unix - b.unix);
  const nowUnix = snapshot?.now_unix || Math.floor(Date.now() / 1000);
  const founded = snapshot?.founded_unix || 0;
  const oldest = sorted.length > 0 ? sorted[0].unix : null;

  const earliestDeed = Object.values(reg.deeds).reduce<number | null>(
    (min, rd) => (min === null || rd.unix < min ? rd.unix : min), null);
  const recordsBegin = earliestDeed !== null && oldest !== null
    ? Math.min(earliestDeed, oldest) : (earliestDeed ?? oldest);
  const truncated = !!founded && recordsBegin !== null
    && recordsBegin - founded > DAY
    && (snapshot?.population?.dead_total || 0) > 0;

  const f: Fold = {
    events: sorted, snapshot, reg, nowUnix, truncated,
    crossUnix: new Map(), lastMatch: new Map(), dirty: false,
  };

  foldCountersAndSets(f);

  // Deeds: already-recorded ones render from the register verbatim; only
  // unrecorded ones are evaluated against the current window.
  for (const def of DEED_DEFS) {
    if (reg.deeds[def.id]) continue;
    let hit: RegDeed | null = null;
    try { hit = def.find(f); } catch { hit = null; }
    if (hit) {
      reg.deeds[def.id] = hit;
      f.dirty = true;
    }
  }

  // Renown tiers — never regress, dated at the crossing
  for (const fam of FAMILY_DEFS) {
    const earned = reg.renown[fam.id] || [];
    for (let tier = earned.length + 1; tier <= fam.tiers.length; tier++) {
      const unix = fam.earnedTier(f, tier);
      if (unix === null) break;
      earned.push({ tier, unix });
      reg.renown[fam.id] = earned;
      f.dirty = true;
    }
  }

  foldEraTurns(f);

  if (f.dirty) saveRegister(colonyId, reg);

  // ---- Build the view ----
  const deeds: Deed[] = DEED_DEFS.map(def => {
    const rec = reg.deeds[def.id];
    if (rec) {
      return {
        id: def.id, arc: def.arc, icon: def.icon, title: def.title,
        state: 'recorded' as DeedState,
        inscription: rec.inscription, unix: rec.unix,
        protagonists: rec.protagonists, callback: rec.callback,
        gentle: def.gentle,
      };
    }
    const revealed = arcRevealed(def.arc, f) && (def.reveal ? def.reveal(f) : true);
    if (!revealed) {
      return { id: def.id, arc: def.arc, icon: def.icon, title: def.title, state: 'unwritten' as DeedState, gentle: def.gentle };
    }
    let hint = '';
    try { hint = def.hint(f); } catch { hint = ''; }
    let progress: number | undefined;
    try { progress = def.progress?.(f); } catch { progress = undefined; }
    return {
      id: def.id, arc: def.arc, icon: def.icon, title: def.title,
      state: 'rumoured' as DeedState, hint, progress, gentle: def.gentle,
    };
  });

  const eras = founded ? computeEras(f, founded) : [];

  const families: RenownFamily[] = FAMILY_DEFS.map(fam => {
    const earnedRaw = reg.renown[fam.id] || [];
    const earned: RenownTier[] = earnedRaw.map(t => ({
      tier: t.tier, unix: t.unix, phrase: fam.tiers[t.tier - 1]?.phrase || '',
    }));
    const nextTier = earned.length + 1;
    return {
      id: fam.id, label: fam.label, earned,
      standing: earned.length > 0 ? earned[earned.length - 1].phrase : undefined,
      nextHint: nextTier <= fam.tiers.length
        ? fam.nextHint(f, nextTier, colonyId)
        : `${colonyId} is “${earned[earned.length - 1]?.phrase}” — the chronicler has nothing left to promise.`,
    };
  });

  // The colony's display title — the most recently earned tier's phrase
  let title: string | null = null;
  let titleUnix = -1;
  for (const fam of families) {
    for (const t of fam.earned) {
      if (t.unix > titleUnix) { titleUnix = t.unix; title = t.phrase; }
    }
  }

  return {
    deeds, eras, families, title,
    kindnessCount: reg.counts['kindness']?.n ?? 0,
    oldestEventUnix: oldest,
    truncated,
  };
}

// ---- Observances -------------------------------------------------------------

function localDateKey(dt: Date): string {
  const y = dt.getFullYear();
  const m = String(dt.getMonth() + 1).padStart(2, '0');
  const day = String(dt.getDate()).padStart(2, '0');
  return `${y}-${m}-${day}`;
}

export function todayKey(): string {
  return localDateKey(new Date());
}

/** Upcoming (and today's) anniversaries, computed from the event window,
 *  the snapshot and the recorded deeds. Sorted soonest-first; within a day,
 *  highest priority first. Horizon ~1 year. */
export function computeObservances(
  events: ColonyEvent[],
  snapshot: ColonySnapshot | null,
  annals: Annals | null,
  today: Date = new Date(),
): Observance[] {
  const out: Observance[] = [];
  const todayStart = new Date(today.getFullYear(), today.getMonth(), today.getDate());
  const startUnix = Math.floor(todayStart.getTime() / 1000);
  const horizonUnix = startUnix + 370 * DAY;
  const push = (unix: number, priority: number, line: string) => {
    if (unix < startUnix || unix > horizonUnix) return;
    out.push({ unix, priority, line, dateKey: localDateKey(new Date(unix * 1000)) });
  };

  // 1a. Anniversary mourning that fired today — the colony itself remembered
  const tKey = localDateKey(todayStart);
  for (const ev of events) {
    if (ev.type !== 'mourning' || !d(ev).anniversary) continue;
    if (localDateKey(new Date(ev.unix * 1000)) !== tKey) continue;
    const dead = String(d(ev).dead_name || 'an old friend');
    const death = events.find(e => e.type === 'death' && e.lilguy === Number(d(ev).dead_id));
    const days = death ? Math.floor((ev.unix - death.unix) / DAY) : 0;
    push(ev.unix, 1,
      `Today ${evName(ev)} walked to ${dead}’s stone.${days > 0 ? ` It has been ${days} days.` : ''}`);
  }

  // 1b. Month-marks of each death (computed even when no firmware event fired)
  const seenDead = new Set<number>();
  for (const ev of events) {
    if (ev.type !== 'death' || seenDead.has(ev.lilguy)) continue;
    seenDead.add(ev.lilguy);
    const name = evName(ev);
    // Next two month-marks (~30-day marks from the death)
    const since = startUnix - ev.unix;
    if (since <= 0) continue;
    const nextMark = Math.max(1, Math.ceil(since / (30 * DAY)));
    for (let k = nextMark; k < nextMark + 2; k++) {
      const unix = ev.unix + k * 30 * DAY;
      push(unix, 1,
        k === 1
          ? `It is a month to the day since the colony lost ${name}.`
          : `It is ${numWord(k)} months to the day since the colony lost ${name}.`);
    }
  }

  // 2. Founding days: weekly for the first four weeks, 30-day marks, then yearly
  const founded = snapshot?.founded_unix || 0;
  if (founded) {
    for (const dNum of [7, 14, 21, 28]) {
      push(founded + dNum * DAY, 2, `The colony is ${numWord(dNum / 7)} week${dNum === 7 ? '' : 's'} old today.`);
    }
    for (let k = 2; k * 30 < 365; k++) {
      push(founded + k * 30 * DAY, 2, `The colony is ${k * 30} days old today.`);
    }
    const YEAR_WORDS = ['one', 'two', 'three', 'four', 'five'];
    for (let y = 1; y <= 5; y++) {
      push(founded + y * 365 * DAY, 2,
        `Founding Day — ${YEAR_WORDS[y - 1] || y} year${y === 1 ? '' : 's'}.`);
    }
  }

  // 3. Yearly anniversaries of recorded Founding-arc deeds
  if (annals) {
    for (const deed of annals.deeds) {
      if (deed.arc !== 'founding' || deed.state !== 'recorded' || !deed.unix || !deed.inscription) continue;
      for (let y = 1; y <= 3; y++) {
        push(deed.unix + y * 365 * DAY, 3,
          `${y === 1 ? 'A year' : `${numWord(y).charAt(0).toUpperCase()}${numWord(y).slice(1)} years`} ago today, ${deed.inscription}.`);
      }
    }
  }

  out.sort((a, b) => a.unix - b.unix || a.priority - b.priority);
  return out;
}

// ---- Home "approaches" selection ---------------------------------------------

/** Up to `limit` rumoured deeds for the Home card. The Long Memory arc never
 *  appears (grief is record, not goal) unless a deed is marked gentle.
 *  Ranked: countable progress ≥ 0.5 nearest-first, then arc order; ties
 *  rotate daily so the card doesn't fossilise. */
export function approachingDeeds(deeds: Deed[], limit = 3): Deed[] {
  const daySeed = Math.floor(Date.now() / (DAY * 1000));
  const eligible = deeds.filter(dd =>
    dd.state === 'rumoured' && (dd.arc !== 'memory' || dd.gentle));
  const score = (dd: Deed): number => {
    if (dd.progress !== undefined && dd.progress >= 0.5) return 1 - dd.progress; // 0..0.5
    return 1 + ARC_ORDER.indexOf(dd.arc);                                        // 1..5
  };
  const jitter = (dd: Deed): number => {
    let h = daySeed;
    for (let i = 0; i < dd.id.length; i++) h = (h * 31 + dd.id.charCodeAt(i)) | 0;
    return (h >>> 0) % 1000;
  };
  return [...eligible]
    .sort((a, b) => score(a) - score(b) || jitter(a) - jitter(b))
    .slice(0, limit);
}
