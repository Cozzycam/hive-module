import type { ColonyEvent } from '../api/types';

/* "While you were away" digest — summarises colony events since the user's
 * last visit into a short narrative. Last-visit timestamp lives in
 * localStorage; the digest only shows when enough time has passed and
 * something actually happened. */

const KEY_LAST_VISIT = 'hive_last_visit_unix';

// Minimum gap before a digest is worth showing
const MIN_AWAY_SECS = 30 * 60;

export function getLastVisitUnix(): number {
  const raw = localStorage.getItem(KEY_LAST_VISIT);
  const v = raw ? parseInt(raw, 10) : 0;
  return Number.isFinite(v) ? v : 0;
}

export function markVisitNow(): void {
  localStorage.setItem(KEY_LAST_VISIT, String(Math.floor(Date.now() / 1000)));
}

export function shouldShowDigest(): boolean {
  const last = getLastVisitUnix();
  if (!last) return false; // first ever visit — nothing to summarise
  return Math.floor(Date.now() / 1000) - last >= MIN_AWAY_SECS;
}

export interface Digest {
  sinceUnix: number;
  awayHours: number;
  births: string[];                       // names
  deaths: { name: string; cause?: string }[];
  foodDeliveries: number;
  foodTotal: number;
  topForager: { name: string; trips: number } | null;
  newBonds: { a: string; b: string }[];
  traitsEarned: { name: string; trait: string }[];
  challenges: string[];                   // descriptions
  milestones: string[];
  parades: { leader: string; joined: number }[];
  isEmpty: boolean;
}

export function computeDigest(
  events: ColonyEvent[],
  sinceUnix: number,
  nameFor: (id: number) => string,
): Digest {
  const within = events.filter(e => e.unix >= sinceUnix);

  const births: string[] = [];
  const deaths: { name: string; cause?: string }[] = [];
  const newBonds: { a: string; b: string }[] = [];
  const traitsEarned: { name: string; trait: string }[] = [];
  const challenges: string[] = [];
  const milestones: string[] = [];
  const parades: { leader: string; joined: number }[] = [];
  let foodDeliveries = 0;
  let foodTotal = 0;
  const foragerTrips = new Map<number, number>();

  for (const ev of within) {
    const data = ev.data as Record<string, unknown>;
    // Prefer the name embedded in the event (survives roster turnover)
    const evName = ((ev as unknown as { name?: string }).name) || nameFor(ev.lilguy);
    switch (ev.type) {
      case 'hatch':
        births.push(evName);
        break;
      case 'death':
        deaths.push({ name: evName, cause: data.cause as string | undefined });
        break;
      case 'food_delivered':
        foodDeliveries++;
        foodTotal += (data.amount as number) || 0;
        if (ev.lilguy) foragerTrips.set(ev.lilguy, (foragerTrips.get(ev.lilguy) || 0) + 1);
        break;
      case 'bond_formed':
        newBonds.push({
          a: evName,
          b: (data.target_name as string)
            || (data.target_id ? nameFor(data.target_id as number) : 'another'),
        });
        break;
      case 'trait_earned':
        traitsEarned.push({ name: evName, trait: String(data.trait || 'unknown') });
        break;
      case 'challenge_start':
        challenges.push(`${String(data.type).replace(/_/g, ' ')} struck`);
        break;
      case 'challenge_end':
        challenges.push(`${String(data.type).replace(/_/g, ' ')} passed`);
        break;
      case 'milestone':
        milestones.push(`${String(data.kind).replace(/_/g, ' ')} (${data.value})`);
        break;
      case 'play':
        parades.push({ leader: evName, joined: Math.max(0, ((data.participants as number) || 2) - 1) });
        break;
      default:
        break;
    }
  }

  let topForager: Digest['topForager'] = null;
  for (const [id, trips] of foragerTrips) {
    if (!topForager || trips > topForager.trips) {
      topForager = { name: nameFor(id), trips };
    }
  }

  const awaySecs = Math.floor(Date.now() / 1000) - sinceUnix;
  const isEmpty = births.length === 0 && deaths.length === 0
    && foodDeliveries === 0 && newBonds.length === 0
    && traitsEarned.length === 0 && challenges.length === 0
    && milestones.length === 0 && parades.length === 0;

  return {
    sinceUnix,
    awayHours: awaySecs / 3600,
    births, deaths, foodDeliveries, foodTotal, topForager,
    newBonds, traitsEarned, challenges, milestones, parades, isEmpty,
  };
}

// Render the digest as short narrative lines, warmest-first.
export function digestLines(d: Digest): { icon: string; text: string }[] {
  const lines: { icon: string; text: string }[] = [];

  if (d.births.length === 1) {
    lines.push({ icon: '\u{1F331}', text: `${d.births[0]} hatched while you were away.` });
  } else if (d.births.length > 1) {
    lines.push({ icon: '\u{1F331}', text: `${joinNames(d.births)} hatched.` });
  }

  for (const death of d.deaths) {
    lines.push({
      icon: '\u{1F342}',
      text: death.cause === 'starvation' || death.cause === 'starved'
        ? `${death.name} starved. The colony needs food.`
        : `${death.name} passed away peacefully.`,
    });
  }

  for (const c of d.challenges) {
    lines.push({ icon: '\u{26A0}\u{FE0F}', text: `A ${c}.` });
  }

  if (d.topForager && d.foodDeliveries >= 3) {
    lines.push({
      icon: '\u{1F36F}',
      text: `${d.foodDeliveries} foraging trips brought in ${d.foodTotal.toFixed(0)}u — ${d.topForager.name} worked hardest (${d.topForager.trips} trips).`,
    });
  } else if (d.foodDeliveries > 0) {
    lines.push({
      icon: '\u{1F36F}',
      text: `${d.foodDeliveries} foraging trip${d.foodDeliveries > 1 ? 's' : ''} brought in ${d.foodTotal.toFixed(0)}u.`,
    });
  }

  for (const bond of d.newBonds.slice(0, 3)) {
    lines.push({ icon: '\u{1F91D}', text: `${bond.a} and ${bond.b} became friends.` });
  }
  if (d.newBonds.length > 3) {
    lines.push({ icon: '\u{1F91D}', text: `…and ${d.newBonds.length - 3} more new friendships.` });
  }

  if (d.parades.length === 1) {
    const p = d.parades[0];
    lines.push({ icon: '\u{1F389}', text: `${p.leader} led a parade — ${p.joined} joined in.` });
  } else if (d.parades.length > 1) {
    lines.push({ icon: '\u{1F389}', text: `${d.parades.length} parades wound through the chamber. Spirits are high.` });
  }

  for (const t of d.traitsEarned) {
    lines.push({ icon: '\u{1F3C5}', text: `${t.name} earned "${t.trait}".` });
  }

  for (const m of d.milestones) {
    lines.push({ icon: '\u{2B50}', text: `Milestone: ${m}.` });
  }

  return lines;
}

function joinNames(names: string[]): string {
  if (names.length <= 1) return names[0] || '';
  if (names.length === 2) return `${names[0]} and ${names[1]}`;
  return `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
}

export function awayLabel(hours: number): string {
  if (hours < 1.5) return 'in the last hour';
  if (hours < 24) return `in the last ${Math.round(hours)} hours`;
  const days = hours / 24;
  if (days < 1.5) return 'since yesterday';
  return `over the last ${Math.round(days)} days`;
}
