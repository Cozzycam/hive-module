import type { Personality } from '../api/types';

// The seven personality dimensions (matching firmware's PersonalityDim enum,
// minus PERS_RESERVE which is unused)
export const PERSONALITY_DIMS = [
  { key: 'work_tempo',       label: 'Tempo',       low: 'Unhurried',  high: 'Driven' },
  { key: 'exploration',      label: 'Exploration',  low: 'Homebody',   high: 'Wanderer' },
  { key: 'route_stickiness', label: 'Routine',      low: 'Improviser',  high: 'Creature of Habit' },
  { key: 'social_frequency', label: 'Sociability',  low: 'Solitary',   high: 'Gregarious' },
  { key: 'food_preference',  label: 'Appetite',     low: 'Ascetic',    high: 'Gourmand' },
  { key: 'hardiness',        label: 'Hardiness',    low: 'Delicate',   high: 'Resilient' },
  { key: 'learning_rate',    label: 'Adaptability', low: 'Steadfast',  high: 'Quick Study' },
] as const;

export type PersonalityKey = typeof PERSONALITY_DIMS[number]['key'];

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
    return 'Balanced temperament';
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

  if (p.social_frequency > 0.7 && p.work_tempo < 0.4) return 'Socialite';
  if (p.exploration > 0.7 && p.route_stickiness < 0.3) return 'Explorer';
  if (p.work_tempo > 0.7 && p.food_preference > 0.6) return 'Forager';
  if (p.work_tempo > 0.7 && p.route_stickiness > 0.6) return 'Labourer';
  if (p.hardiness > 0.7) return 'Hardy';
  if (p.social_frequency > 0.6 && p.learning_rate > 0.6) return 'Nurturer';
  if (p.exploration < 0.3 && p.route_stickiness > 0.6) return 'Homeguard';

  return capitalize(role);
}

function capitalize(s: string): string {
  return s.charAt(0).toUpperCase() + s.slice(1);
}
