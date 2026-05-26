// Hive colour palette — warm, earthy, calm
export const HIVE = {
  cream:      '#E9DCC1',
  parchment:  '#DDD0B4',
  sand:       '#C9B896',
  bark:       '#8B7355',
  soil:       '#5C4A32',
  soot:       '#3D342A',
  ink:        '#2B241D',
  white:      '#F5ECDD',
  dimText:    '#9E8E74',
  accent:     '#D9A54C',  // golden amber
  alert:      '#DC6038',
  green:      '#5A8A4A',
  leafGreen:  '#6B9E50',
  sky:        '#7ABED4',
  night:      '#1E2A38',
};

// Time-of-day palette overrides
export interface TodPalette {
  bg: string;
  cardBg: string;
  text: string;
  dimText: string;
  accent: string;
  headerBg: string;
}

export const TOD_PALETTES: Record<string, TodPalette> = {
  day: {
    bg: HIVE.cream,
    cardBg: '#F5ECDD',
    text: HIVE.ink,
    dimText: HIVE.dimText,
    accent: HIVE.accent,
    headerBg: HIVE.cream,
  },
  dawn: {
    bg: '#E8D8C4',
    cardBg: '#F0E4D2',
    text: HIVE.ink,
    dimText: '#9E8E74',
    accent: '#D4956A',
    headerBg: '#E8D8C4',
  },
  dusk: {
    bg: '#D8C8AA',
    cardBg: '#E4D6BE',
    text: HIVE.ink,
    dimText: '#8A7A60',
    accent: '#C48850',
    headerBg: '#D8C8AA',
  },
  night: {
    bg: '#2A3440',
    cardBg: '#354250',
    text: '#D4CCBA',
    dimText: '#8A94A6',
    accent: '#B89660',
    headerBg: '#222C38',
  },
};

// Interpolate between day and night palette using night_factor
export function interpolatePalette(phase: string, nightFactor: number): TodPalette {
  // Use phase directly for dawn/dusk; interpolate for edge cases
  if (phase === 'dawn') return TOD_PALETTES.dawn;
  if (phase === 'dusk') return TOD_PALETTES.dusk;
  if (phase === 'night') return TOD_PALETTES.night;
  return TOD_PALETTES.day;
}

// Connection indicator colours
export const CONNECTION_COLORS = {
  lan: '#5A8A4A',    // green — LAN-fresh
  vps: '#D9A54C',    // amber — VPS-fresh
  cache: '#9E8E74',  // grey — stale cache
  none: '#DC6038',   // red — no data
};
