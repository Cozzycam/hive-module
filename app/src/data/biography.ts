// Life stories and epithets — the Dwarf-Fortress-historian layer.
// Turns a conker's event history into a readable biography, and hangs an
// earned title off their name ("Bramble the Bold") so characters are
// memorable, not just rows. (Amber feedback: "title for little guys who
// catch things"; the goal: "remember Bramble, the cheeky one?")
import type { ColonyEvent, Personality } from '../api/types';
import { nameFromId } from './plantNames';
import { traitLabel, SURVIVAL_TRAITS } from './traits';
import { CRITTER_PLURALS } from './critters';

// ---- Epithets ----------------------------------------------------------

const SURVIVAL_EPITHETS: Record<string, string> = {
  survived_storm: 'the Storm-Tested',
  survived_heatwave: 'the Sun-Scorched',
  survived_cold_snap: 'the Frost-Hardy',
  survived_drought: 'the Dust-Weathered',
};

// Personality extremes → flavour epithets (checked high then low).
// Order = precedence among personality dims when several are extreme.
const DIM_EPITHETS: { key: keyof Personality; high: string; low: string }[] = [
  { key: 'bravery', high: 'the Bold', low: 'the Timid' },
  { key: 'playfulness', high: 'the Merry', low: 'the Solemn' },
  { key: 'exploration', high: 'the Wanderer', low: 'the Homebird' },
  { key: 'work_tempo', high: 'the Tireless', low: 'the Dozy' },
  { key: 'social_frequency', high: 'the Friendly', low: 'the Quiet' },
  { key: 'appetite', high: 'the Hungry', low: 'the Sparrow' },
];

/** Earned titles trump flavour; flavour only for genuinely extreme dims.
 *  events is optional — the roster list can compute epithets from traits +
 *  personality alone; the profile refines with history. */
export function epithet(
  traits: string[],
  personality?: Personality,
  events?: ColonyEvent[],
): string | null {
  if (traits.includes('catcher')) return 'the Bug Hunter';

  const survived = traits.filter(t => SURVIVAL_TRAITS.includes(t));
  if (survived.length >= 2) return 'the Unbreakable';
  if (survived.length === 1) return SURVIVAL_EPITHETS[survived[0]] ?? null;

  if (events && events.length > 0) {
    const works = events.filter(e => e.type === 'crafted').length;
    if (works >= 3) return 'the Maker';
    const sows = events.filter(e => e.type === 'crop_sown').length;
    if (sows >= 10) return 'the Green-Thumbed';
    const parades = events.filter(e => e.type === 'play').length;
    if (parades >= 5) return 'the Parade-Leader';
    const finds = events.filter(e => e.type === 'discovery').length;
    if (finds >= 10) return 'the Keen-Eyed';
  }

  if (traits.includes('elder')) return 'the Elder';

  if (personality) {
    for (const d of DIM_EPITHETS) {
      const v = personality[d.key];
      if (v === undefined) continue;
      if (v >= 0.85) return d.high;
      if (v <= 0.15) return d.low;
    }
  }
  return null;
}

// ---- Life story ---------------------------------------------------------

export interface StoryEntry {
  unix: number;
  icon: string;
  text: string;
}

interface StorySubject {
  id: number;
  name: string;
  founder?: boolean;
  died_unix?: number;
  age_days?: number;
  born_unix?: number;
  tended_by_name?: string;
}


function listNames(names: string[]): string {
  if (names.length === 1) return names[0];
  return `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
}

/** Build a chronological biography from a conker's own events, plus
 *  colony-wide events for the parts of their story other conkers carry
 *  (who mourned them). Duplicate beats (bonds re-forming after decay)
 *  keep only their first telling. */
export function buildLifeStory(
  subject: StorySubject,
  events: ColonyEvent[],
  rosterNames: Map<number, string>,
  allEvents?: ColonyEvent[],
): StoryEntry[] {
  const story: StoryEntry[] = [];
  const resolveName = (ev: ColonyEvent, id: unknown): string => {
    const data = ev.data as Record<string, unknown>;
    return (data.target_name as string)
      || (typeof id === 'number' ? (rosterNames.get(id) || nameFromId(id)) : 'a friend');
  };

  // Birth — from detail if known, else the hatch event
  const hatch = events.find(e => e.type === 'hatch');
  const bornUnix = subject.born_unix || hatch?.unix;
  if (bornUnix) {
    story.push({
      unix: bornUnix,
      icon: '\u{1F331}',
      text: subject.founder
        ? 'Hatched in the founding days of the colony.'
        : 'Hatched.',
    });
    if (subject.tended_by_name) {
      story.push({
        unix: bornUnix,
        icon: '\u{1F9B7}',
        text: `Raised by ${subject.tended_by_name} — and takes after them.`,
      });
    }
  }

  const seenBonds = new Set<string>();
  const discoveries: Record<string, number> = {};
  let lastDiscoveryUnix = 0;
  let parades = 0;
  let lastParadeUnix = 0;
  let sows = 0;
  let lastSowUnix = 0;
  let works = 0;
  let lastWorkUnix = 0;

  for (const ev of events) {
    const data = ev.data as Record<string, unknown>;
    switch (ev.type) {
      case 'bond_formed': {
        const key = `f${data.target_id}`;
        if (seenBonds.has(key)) break;
        seenBonds.add(key);
        story.push({ unix: ev.unix, icon: '\u{1F91D}', text: `Took a shine to ${resolveName(ev, data.target_id)}.` });
        break;
      }
      case 'became_best_friends': {
        const key = `m${data.target_id}`;
        if (seenBonds.has(key)) break;
        seenBonds.add(key);
        story.push({ unix: ev.unix, icon: '\u{2B50}', text: `Became best friends with ${resolveName(ev, data.target_id)}.` });
        break;
      }
      case 'bond_broken':
        story.push({ unix: ev.unix, icon: '\u{1F4A8}', text: `Drifted apart from ${resolveName(ev, data.target_id)}.` });
        break;
      case 'trait_earned':
        story.push({ unix: ev.unix, icon: '\u{1F3C5}', text: `Earned "${traitLabel(String(data.trait))}".` });
        break;
      case 'mourning': {
        const dead = (data.dead_name as string)
          || (typeof data.dead_id === 'number'
              ? (rosterNames.get(data.dead_id as number) || nameFromId(data.dead_id as number))
              : 'a fallen friend');
        story.push({
          unix: ev.unix,
          icon: '\u{1F56F}\u{FE0F}',
          text: data.anniversary
            ? `Visited ${dead}'s stone, remembering.`
            : `Stood vigil for ${dead}.`,
        });
        break;
      }
      case 'discovery': {
        const critter = String(data.critter || 'critter');
        discoveries[critter] = (discoveries[critter] || 0) + 1;
        lastDiscoveryUnix = ev.unix;
        break;
      }
      case 'play': {
        parades++;
        lastParadeUnix = ev.unix;
        break;
      }
      case 'crop_sown': {
        sows++;
        lastSowUnix = ev.unix;
        break;
      }
      case 'crafted': {
        // Memorials are named individually — they're acts of love, not
        // output; other works aggregate into an oeuvre line below
        if (data.honoree) {
          story.push({
            unix: ev.unix,
            icon: '\u{1FAA6}',
            text: `Carved a memorial for ${data.honoree}.`,
          });
        } else {
          works++;
          lastWorkUnix = ev.unix;
        }
        break;
      }
      case 'death':
        story.push({
          unix: ev.unix,
          icon: '\u{1F342}',
          text: `Passed away${subject.age_days ? ` at ${subject.age_days.toFixed(0)} days old` : ''}`
            + `${data.cause === 'starvation' ? ', hungry to the last' : ', of a grand old age'}`
            + `${data.attended_by ? ` — ${data.attended_by} was at their side` : ''}.`,
        });
        break;
      default:
        break;
    }
  }

  // Aggregates — one line each, placed at their most recent occurrence
  const critterKinds = Object.keys(discoveries);
  if (critterKinds.length > 0) {
    const total = critterKinds.reduce((s, k) => s + discoveries[k], 0);
    const parts = critterKinds.map(k =>
      `${discoveries[k]} ${discoveries[k] === 1 ? k : (CRITTER_PLURALS[k] || k + 's')}`);
    story.push({
      unix: lastDiscoveryUnix,
      icon: '\u{1F50D}',
      text: `Has found ${total === 1 ? parts[0] : listNames(parts)} so far.`,
    });
  }
  if (parades > 0) {
    story.push({
      unix: lastParadeUnix,
      icon: '\u{1F389}',
      text: parades === 1 ? 'Once led a parade around the chamber.'
                          : `Has led ${parades} parades around the chamber.`,
    });
  }
  if (sows > 0) {
    story.push({
      unix: lastSowUnix,
      icon: '\u{1F33E}',
      text: sows === 1 ? 'Sowed their first crop in the garden.'
                       : `Has sown ${sows} crops in the garden.`,
    });
  }
  if (works > 0) {
    story.push({
      unix: lastWorkUnix,
      icon: '\u{1F3A8}',
      text: works === 1 ? 'Made something that will outlast the day.'
                        : `Has made ${works} works — the chamber carries their colours.`,
    });
  }

  // Who mourned them — carried by the mourners' events, not their own
  if (subject.died_unix && allEvents) {
    const mourners = allEvents
      .filter(e => e.type === 'mourning'
        && (e.data as Record<string, unknown>).dead_id === subject.id)
      .map(e => (e as unknown as { name?: string }).name
        || rosterNames.get(e.lilguy) || nameFromId(e.lilguy));
    if (mourners.length > 0) {
      story.push({
        unix: subject.died_unix,
        icon: '\u{1F490}',
        text: `Mourned by ${listNames([...new Set(mourners)])}.`,
      });
    }
  }

  return story.sort((a, b) => a.unix - b.unix);
}
