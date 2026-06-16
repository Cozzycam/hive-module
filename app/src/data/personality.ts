import type { Personality } from '../api/types';

// The seven personality dimensions (matching firmware's PersonalityDim enum).
// Language rule (Amber feedback): simple, cute, understandable — and every
// dimension says what it actually means for how the conker behaves.
export const PERSONALITY_DIMS = [
  {
    key: 'work_tempo', label: 'Energy',
    low: 'Dozy', high: 'Busy bee',
    meaning: 'How hard they work. Busy bees forage more and rest less; dozy ones nap and potter.',
  },
  {
    key: 'exploration', label: 'Curiosity',
    low: 'Homebird', high: 'Adventurer',
    meaning: 'How far they roam. Adventurers find food first — and are the ones who start the zoomies.',
  },
  {
    key: 'route_stickiness', label: 'Habits',
    low: 'Free spirit', high: 'Routine lover',
    meaning: 'Routine lovers walk the same trusty trails every time; free spirits try new ways.',
  },
  {
    key: 'social_frequency', label: 'Friendliness',
    low: 'Quiet one', high: 'Social butterfly',
    meaning: 'How often they greet, share food and play. The friendly ones make best friends fastest.',
  },
  {
    key: 'food_preference', label: 'Appetite',
    low: 'Light eater', high: 'Big eater',
    meaning: 'Big eaters snack more often — keep the larder topped up for them.',
  },
  {
    key: 'hardiness', label: 'Toughness',
    low: 'Soft shell', high: 'Tough nut',
    meaning: 'Tough nuts shrug off heatwaves, cold snaps and hungry days better than most.',
  },
  {
    key: 'learning_rate', label: 'Smarts',
    low: 'Old soul', high: 'Fast learner',
    meaning: 'Fast learners pick up new routes and tricks quickly; old souls do things the old way.',
  },
] as const;

export type PersonalityKey = typeof PERSONALITY_DIMS[number]['key'];

// Which pole (or middle) a conker sits on for a dimension
export function dimLeaning(dim: typeof PERSONALITY_DIMS[number], val: number): string {
  if (val >= 0.6) return dim.high;
  if (val <= 0.4) return dim.low;
  return 'In between';
}

// Derive a one-line personality phrase from the vector
export function personalityPhrase(p: Personality): string {
  const vals = PERSONALITY_DIMS.map(d => ({
    dim: d,
    val: p[d.key as keyof Personality],
  }));

  // Find the most extreme dimension (furthest from 0.5)
  const sorted = [...vals].sort((a, b) => Math.abs(b.val - 0.5) - Math.abs(a.val - 0.5));
  const top = sorted[0];

  if (Math.abs(top.val - 0.5) < 0.15) {
    return 'A bit of everything';
  }

  const label = top.val > 0.5 ? top.dim.high : top.dim.low;
  const second = sorted[1];
  if (Math.abs(second.val - 0.5) > 0.2) {
    const secondLabel = second.val > 0.5 ? second.dim.high : second.dim.low;
    return `${label}, ${secondLabel.toLowerCase()}`;
  }

  return label;
}

// Derive an emergent role tag from personality + traits
export function deriveRoleTag(
  p: Personality | undefined,
  role: string,
  traits: string[],
): string {
  if (traits.includes('pioneer')) return 'Pioneer';
  if (traits.includes('elder')) return 'Elder';

  if (!p) return capitalize(role);

  // Tag a conker by its single most-pronounced trait — so everyone gets a name
  // that fires as readily as "The tough one", not just the rare double-extreme.
  const dims: Array<[number, string]> = [
    [p.hardiness, 'The tough one'],
    [p.work_tempo, 'The grafter'],
    [p.exploration, 'The explorer'],
    [p.social_frequency, 'The social one'],
    [p.food_preference, 'The forager'],
    [p.route_stickiness, 'The homebody'],
    [p.learning_rate, 'The clever one'],
  ];
  let best = dims[0];
  for (const d of dims) if (d[0] > best[0]) best = d;
  // Only fall back to "all-rounder" for a genuinely flat personality.
  if (best[0] >= 0.55) return best[1];
  return 'The all-rounder';
}

function capitalize(s: string): string {
  return s.charAt(0).toUpperCase() + s.slice(1);
}
