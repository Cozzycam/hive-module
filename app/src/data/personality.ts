import type { Personality } from '../api/types';

// The eight personality dimensions (matching firmware's PersonalityDim enum).
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
    key: 'appetite', label: 'Appetite',
    low: 'Light eater', high: 'Big eater',
    meaning: 'Big eaters snack sooner and keep food for themselves; light eaters share their load with hungry nestmates.',
  },
  {
    key: 'hardiness', label: 'Toughness',
    low: 'Soft shell', high: 'Tough nut',
    meaning: 'Tough nuts shrug off heatwaves, cold snaps and hungry days better than most.',
  },
  {
    key: 'playfulness', label: 'Playfulness',
    low: 'Serious', high: 'Playful',
    meaning: 'Playful ones romp and zoom for the joy of it; serious ones only play when they get really bored.',
  },
  {
    key: 'bravery', label: 'Bravery',
    low: 'Timid', high: 'Brave',
    meaning: 'Brave ones chase fireflies and lead the parades; timid ones keep near the queen and follow along.',
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

  // Compound tag: NOUN from the strongest trait, ADJECTIVE from the second
  // strongest. ~42 combinations, so three high-homebody conkers read as
  // "The hungry homebody" / "The sociable homebody" / "The tough homebody"
  // instead of three identical "The homebody" — distinct AND accurate.
  const dims: Array<{ v: number; noun: string; adj: string }> = [
    { v: p.work_tempo,       noun: 'grafter',   adj: 'busy' },
    { v: p.exploration,      noun: 'explorer',  adj: 'curious' },
    { v: p.route_stickiness, noun: 'homebody',  adj: 'homely' },
    { v: p.social_frequency, noun: 'socialite', adj: 'sociable' },
    { v: p.appetite,         noun: 'forager',   adj: 'hungry' },
    { v: p.hardiness,        noun: 'tough nut', adj: 'tough' },
    { v: p.playfulness,      noun: 'joker',     adj: 'playful' },
    { v: p.bravery,          noun: 'daredevil', adj: 'brave' },
  ];
  const ranked = dims.slice().sort((a, b) => b.v - a.v);
  // Genuinely flat personality → no standout trait.
  if (ranked[0].v < 0.55) return 'The all-rounder';
  return `The ${ranked[1].adj} ${ranked[0].noun}`;
}

function capitalize(s: string): string {
  return s.charAt(0).toUpperCase() + s.slice(1);
}
