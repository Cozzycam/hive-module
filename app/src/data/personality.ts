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
  if (traits.includes('Pioneer')) return 'Pioneer';
  if (traits.includes('Elder')) return 'Elder';

  if (!p) return capitalize(role);

  if (p.social_frequency > 0.7 && p.work_tempo < 0.4) return 'The social one';
  if (p.exploration > 0.7 && p.route_stickiness < 0.3) return 'The explorer';
  if (p.work_tempo > 0.7 && p.food_preference > 0.6) return 'The forager';
  if (p.work_tempo > 0.7 && p.route_stickiness > 0.6) return 'The grafter';
  if (p.hardiness > 0.7) return 'The tough one';
  if (p.social_frequency > 0.6 && p.learning_rate > 0.6) return 'The carer';
  if (p.exploration < 0.3 && p.route_stickiness > 0.6) return 'The homebody';

  return capitalize(role);
}

function capitalize(s: string): string {
  return s.charAt(0).toUpperCase() + s.slice(1);
}
