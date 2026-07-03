// Artifact display metadata — keep in lockstep with ArtKind / ArtContext
// (firmware chamber.h). Works are reconstructed from crafted/art_weathered
// events; the event log is the gallery's source of truth.
import type { ColonyEvent } from '../api/types';

export function artKindEmoji(kind: string): string {
  switch (kind) {
    case 'sculpture':    return '\u{1F5FF}';
    case 'cairn':        return '\u{1FAA8}';
    case 'painting':     return '\u{1F3A8}';
    case 'memorial':     return '\u{1FAA6}';
    case 'petal_hat':    return '\u{1F338}';
    case 'seed_pendant': return '\u{1F4FF}';
    case 'grass_band':   return '\u{1F33F}';
    default:             return '\u{2728}';
  }
}

export function artKindLabel(kind: string): string {
  return kind.replace(/_/g, ' ');
}

export function isAccessory(kind: string): boolean {
  return kind === 'petal_hat' || kind === 'seed_pendant' || kind === 'grass_band';
}

export function artContextPhrase(context: string, honoree?: string): string {
  if (honoree) return `in memory of ${honoree}`;
  switch (context) {
    case 'storm':    return 'as a storm battered the colony';
    case 'heatwave': return 'in a scorching spell';
    case 'rain':     return 'on a rain-soaked day';
    case 'night':    return 'by firefly light';
    case 'grief':    return 'in a time of grief';
    default:         return 'in a time of plenty';
  }
}

// Kind-aware provenance: memorials remember, accessories are for someone,
// everything else carries the weather of its making
export function artProvenance(kind: string, context: string, honoree?: string): string {
  if (isAccessory(kind)) return honoree ? `for ${honoree}` : 'as a gift';
  return artContextPhrase(context, kind === 'memorial' ? honoree : undefined);
}

export interface GalleryWork {
  kind: string;
  maker: string;
  makerId: number;
  context: string;
  honoree?: string;
  unix: number;
  weathered: boolean;
}

// Rebuild the gallery from history: each crafted event is a work; each
// art_weathered retires the OLDEST standing work matching maker + kind.
export function buildGallery(events: ColonyEvent[]): GalleryWork[] {
  const works: GalleryWork[] = [];
  const sorted = [...events].sort((a, b) => a.unix - b.unix);
  for (const ev of sorted) {
    const data = ev.data as Record<string, unknown>;
    const name = (ev as unknown as { name?: string }).name || 'someone';
    if (ev.type === 'crafted') {
      works.push({
        kind: String(data.kind || 'work'),
        maker: name,
        makerId: ev.lilguy,
        context: String(data.context || 'plenty'),
        honoree: data.honoree ? String(data.honoree) : undefined,
        unix: ev.unix,
        weathered: false,
      });
    } else if (ev.type === 'art_weathered') {
      const victim = works.find(w =>
        !w.weathered && w.maker === name && w.kind === String(data.kind || ''));
      if (victim) victim.weathered = true;
    }
  }
  return works.reverse();  // newest first
}
