// The Saga — the colony's history composed as prose chapters, one per
// week. This is the Dwarf-Fortress-historian payoff: not a log but a
// story, with named characters, callbacks to earlier deeds, and weeks
// that get titles from whatever mattered most in them.
import type { ColonyEvent } from '../api/types';
import { traitLabel } from './traits';
import { artKindLabel, isAccessory } from './artworks';
import { nameFromId } from './plantNames';

export interface Chapter {
  title: string;
  startUnix: number;
  paragraphs: string[];
}

const WEEK = 7 * 86400;

const ORDINALS = [
  'first', 'second', 'third', 'fourth', 'fifth', 'sixth', 'seventh',
  'eighth', 'ninth', 'tenth', 'eleventh', 'twelfth',
];
function ordinal(n: number): string {
  return ORDINALS[n] ?? `${n + 1}th`;
}

function listNames(names: string[]): string {
  const u = [...new Set(names)];
  if (u.length === 1) return u[0];
  return `${u.slice(0, -1).join(', ')} and ${u[u.length - 1]}`;
}

function evName(ev: ColonyEvent): string {
  return (ev as unknown as { name?: string }).name
    || (ev.lilguy ? nameFromId(ev.lilguy) : 'someone');
}

function targetName(ev: ColonyEvent): string {
  const data = ev.data as Record<string, unknown>;
  return (data.target_name as string)
    || (typeof data.target_id === 'number' ? nameFromId(data.target_id) : 'a nestmate');
}

// ---- The composer ----

export function composeSaga(events: ColonyEvent[], foundedUnix?: number): Chapter[] {
  const sorted = [...events]
    .filter(e => e.type !== 'chamber_crossing')
    .sort((a, b) => a.unix - b.unix);
  if (sorted.length === 0) return [];

  const start = foundedUnix || sorted[0].unix;
  const weeks = new Map<number, ColonyEvent[]>();
  for (const e of sorted) {
    const w = Math.max(0, Math.floor((e.unix - start) / WEEK));
    if (!weeks.has(w)) weeks.set(w, []);
    weeks.get(w)!.push(e);
  }

  // The callback engine: remembered deeds, woven back in when a name
  // resurfaces at a moment that matters (chiefly: their death).
  const deeds = new Map<string, string>();
  let firstCritterFound = false;

  const chapters: Chapter[] = [];
  const weekIdxs = [...weeks.keys()].sort((a, b) => a - b);
  for (const w of weekIdxs) {
    chapters.push(composeChapter(w, weeks.get(w)!, start, deeds, {
      firstCritterFound: () => firstCritterFound,
      markFirstCritter: () => { firstCritterFound = true; },
    }));
  }
  return chapters.reverse();  // newest chapter first
}

interface FirstFlags {
  firstCritterFound: () => boolean;
  markFirstCritter: () => void;
}

function composeChapter(
  w: number,
  events: ColonyEvent[],
  sagaStart: number,
  deeds: Map<string, string>,
  firsts: FirstFlags,
): Chapter {
  const paragraphs: string[] = [];
  const d = (ev: ColonyEvent) => ev.data as Record<string, unknown>;

  const births = events.filter(e => e.type === 'hatch');
  const deaths = events.filter(e => e.type === 'death');
  const bffs = events.filter(e => e.type === 'became_best_friends');
  const breaks = events.filter(e => e.type === 'bond_broken');
  const traits = events.filter(e => e.type === 'trait_earned');
  const challenges = events.filter(e => e.type === 'challenge_start');
  const challengeEnds = events.filter(e => e.type === 'challenge_end');
  const crafted = events.filter(e => e.type === 'crafted');
  const mournings = events.filter(e => e.type === 'mourning');
  const discoveries = events.filter(e => e.type === 'discovery');
  const sows = events.filter(e => e.type === 'crop_sown');
  const parades = events.filter(e => e.type === 'play');
  const gifts = events.filter(e => e.type === 'colony_event'
    && (d(e) as { kind?: string }).kind === 'care_package_from_neighbours');

  // --- Opening + births ---
  {
    const opener = w === 0
      ? 'In the colony’s first week,'
      : `As the ${ordinal(w)} week opened,`;
    if (births.length > 0) {
      const founders = births.filter(e => d(e).is_pioneer || w === 0);
      const names = [...new Set(births.map(evName))];
      paragraphs.push(
        `${opener} ${names.length === 1
          ? `${names[0]} hatched${w === 0 ? ' into the founding days' : ''}.`
          : `${names.length} new faces hatched: ${listNames(names)}.`}`
        + (founders.length > 0 && w === 0
          ? ' These were the founders — the ones every later story leads back to.'
          : ''),
      );
    } else if (w === 0) {
      paragraphs.push('In the colony’s first week, the queen laid her founding eggs and the chamber waited.');
    } else {
      paragraphs.push(`The ${ordinal(w)} week came in quietly.`);
    }
  }

  // --- Trials (challenges) ---
  for (const c of challenges) {
    const kind = String(d(c).type || 'challenge').replace(/_/g, ' ');
    const ended = challengeEnds.find(e => e.unix > c.unix);
    const survivors = traits.filter(t =>
      String(d(t).trait || '').startsWith('survived_') && t.unix >= c.unix).length;
    paragraphs.push(
      `A ${kind} ${kind === 'storm' ? 'battered' : 'gripped'} the colony`
      + (ended ? ` — and passed. ` : `. `)
      + (survivors > 0
        ? `${survivors === 1 ? 'One came' : `${survivors} came`} through it, and will not forget.`
        : ''),
    );
  }

  // --- Deaths, each honoured, with callbacks ---
  for (const death of deaths) {
    const name = evName(death);
    const deed = deeds.get(name);
    const attended = d(death).attended_by as string | undefined;
    const cause = d(death).cause === 'starvation' ? ' in a hungry season' : '';
    const memorial = crafted.find(e =>
      String(d(e).kind) === 'memorial' && String(d(e).honoree) === name);
    const vigils = mournings.filter(e =>
      !d(e).anniversary && String(d(e).dead_name || '') === name);
    let line = `The week took ${name}${deed ? ` — ${deed}` : ''}${cause}`;
    line += attended ? `, with ${attended} at their side.` : '.';
    if (vigils.length > 0)
      line += ` ${listNames(vigils.map(evName))} stood vigil.`;
    if (memorial)
      line += ` ${evName(memorial)} carved the memorial that stands still.`;
    paragraphs.push(line);
    deeds.set(name, 'who is remembered');
  }

  // --- Friendships ---
  if (bffs.length > 0) {
    const pairs = bffs.map(e => `${evName(e)} and ${targetName(e)}`);
    paragraphs.push(
      bffs.length === 1
        ? `${pairs[0]} became inseparable.`
        : `Friendships sealed themselves: ${pairs.join('; ')}.`,
    );
    for (const e of bffs) {
      if (!deeds.has(evName(e)))
        deeds.set(evName(e), `bound to ${targetName(e)} since the ${ordinal(w)} week`);
    }
  }
  if (breaks.length > 0) {
    paragraphs.push(`${breaks.map(e =>
      `${evName(e)} and ${targetName(e)}`).join('; ')} drifted apart.`);
  }

  // --- Titles (championship changes aggregate — the pre-v179 archives
  // have thrash eras where the crown moved daily) ---
  const catcherEarns = traits.filter(t => String(d(t).trait) === 'catcher');
  if (catcherEarns.length === 1) {
    const name = evName(catcherEarns[0]);
    paragraphs.push(`${name} took the Bug Hunter title.`);
    deeds.set(name, `Bug Hunter since the ${ordinal(w)} week`);
  } else if (catcherEarns.length > 1) {
    const last = evName(catcherEarns[catcherEarns.length - 1]);
    paragraphs.push(
      `The Bug Hunter title changed hands ${catcherEarns.length} times — ${last} held it when the week closed.`,
    );
    deeds.set(last, `Bug Hunter since the ${ordinal(w)} week`);
  }
  for (const t of traits) {
    const trait = String(d(t).trait || '');
    if (trait === 'catcher' || trait.startsWith('survived_')) continue;
    const name = evName(t);
    if (trait === 'elder') deeds.set(name, 'an elder of the colony');
    else if (!deeds.has(name))
      deeds.set(name, `“${traitLabel(trait)}” since the ${ordinal(w)} week`);
  }

  // --- Works & gifts ---
  const placedWorks = crafted.filter(e =>
    !isAccessory(String(d(e).kind)) && String(d(e).kind) !== 'memorial');
  if (placedWorks.length > 0) {
    const first = placedWorks[0];
    paragraphs.push(
      placedWorks.length === 1
        ? `The muse visited ${evName(first)}, whose ${artKindLabel(String(d(first).kind))} joined the chamber.`
        : `The makers were busy: ${listNames(placedWorks.map(evName))} left new works in the chamber.`,
    );
    for (const e of placedWorks) {
      if (!deeds.has(evName(e)))
        deeds.set(evName(e), `maker of the ${ordinal(w)}-week ${artKindLabel(String(d(e).kind))}`);
    }
  }
  const givenGifts = crafted.filter(e => isAccessory(String(d(e).kind)));
  for (const g of givenGifts) {
    paragraphs.push(
      `${evName(g)} made a ${artKindLabel(String(d(g).kind))} for ${String(d(g).honoree || 'a friend')} — worn ever since.`,
    );
  }

  // --- Remembering (anniversary visits) ---
  const anniversaries = mournings.filter(e => d(e).anniversary);
  for (const a of anniversaries) {
    paragraphs.push(
      `${evName(a)} walked to ${String(d(a).dead_name || 'an old friend')}’s stone again, as they do.`,
    );
  }

  // --- Ambient texture, one line ---
  {
    const bits: string[] = [];
    if (discoveries.length > 0) {
      const counts: Record<string, number> = {};
      for (const e of discoveries) {
        const k = String(d(e).critter || 'critter');
        counts[k] = (counts[k] || 0) + 1;
      }
      const total = discoveries.length;
      bits.push(total > 50
        ? 'visitors beyond counting drifted through'
        : `${total} ${total === 1 ? 'visitor was' : 'visitors were'} spotted`);
      if (!firsts.firstCritterFound()) {
        const first = discoveries[0];
        paragraphs.push(
          `${evName(first)} found the colony’s first ${String(d(first).critter || 'visitor')} — the Field Guide begins with them.`,
        );
        if (!deeds.has(evName(first)))
          deeds.set(evName(first), `who found the colony’s first ${String(d(first).critter || 'visitor')}`);
        firsts.markFirstCritter();
      }
    }
    if (sows.length > 0) bits.push(`${sows.length} crop${sows.length === 1 ? '' : 's'} sown in the garden`);
    if (parades.length > 0) bits.push(`parades most days`);
    if (gifts.length > 0) bits.push(`${gifts.length} care package${gifts.length === 1 ? '' : 's'} from beyond the border`);
    if (bits.length > 0)
      paragraphs.push(`Between times: ${bits.join(', ')}.`);
  }

  // --- Chapter title: the week's headline ---
  let title: string;
  if (deaths.length > 0)       title = `The Week We Lost ${listNames(deaths.map(evName))}`;
  else if (challenges.length > 0) {
    const kind = String(d(challenges[0]).type || 'storm').replace(/_/g, ' ');
    title = `The Week the ${kind.charAt(0).toUpperCase() + kind.slice(1)} Came`;
  }
  else if (bffs.length > 0)    title = 'The Week of Friendships';
  else if (givenGifts.length > 0) title = 'The Week of Gifts';
  else if (placedWorks.length > 0) title = 'The Week the Muse Visited';
  else if (births.length > 1)  title = 'The Week of New Faces';
  else if (w === 0)            title = 'The Founding';
  else                          title = 'A Quiet Week';

  return {
    title: `Chapter ${w + 1} — ${title}`,
    startUnix: sagaStart + w * WEEK,
    paragraphs,
  };
}
