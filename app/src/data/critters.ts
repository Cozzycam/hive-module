// Shared critter display metadata — one source of truth for the emoji and
// plural maps that used to be duplicated across Diary, App toast and
// biography. Keep in lockstep with CritterKind (firmware chamber.h).
export function critterEmoji(kind: string): string {
  switch (kind) {
    case 'butterfly': return '\u{1F98B}';
    case 'worm':      return '\u{1FAB1}';
    case 'firefly':   return '\u{2728}';
    case 'beetle':    return '\u{1FAB2}';
    case 'moth':      return '\u{1F319}';
    case 'snail':     return '\u{1F40C}';
    case 'ladybird':  return '\u{1F41E}';
    case 'dragonfly': return '\u{1F4A0}';
    default:          return '\u{1F50D}';
  }
}

export const CRITTER_PLURALS: Record<string, string> = {
  butterfly: 'butterflies', worm: 'worms', firefly: 'fireflies',
  beetle: 'beetles', moth: 'moths', snail: 'snails',
  ladybird: 'ladybirds', dragonfly: 'dragonflies',
};

export function critterPlural(kind: string, n: number): string {
  return n === 1 ? kind : (CRITTER_PLURALS[kind] || kind + 's');
}
