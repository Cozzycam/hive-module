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

// The concrete form a work took — mirrors the firmware's sprite pick
// (renderer.cpp _draw_artworks): sculpture motif%3, painting motif%2.
// 'lost' = the event predates the motif field, so the form went unrecorded;
// null = accessories and unknown kinds, which have no placed form.
export type ArtForm =
  | 'orb' | 'spire' | 'arch'
  | 'paint_diamonds' | 'paint_meander'
  | 'cairn' | 'memorial'
  | 'lost'
  | null;

export function artForm(kind: string, motif?: number): ArtForm {
  switch (kind) {
    case 'sculpture':
      if (motif === undefined) return 'lost';
      return (['orb', 'spire', 'arch'] as const)[motif % 3];
    case 'painting':
      if (motif === undefined) return 'lost';
      return motif % 2 ? 'paint_meander' : 'paint_diamonds';
    case 'cairn':    return 'cairn';
    case 'memorial': return 'memorial';
    default:         return null;
  }
}

// A written portrait of the piece — form + kind + provenance — so the
// gallery reads like a catalogue instead of a list of coloured things.
export function artDescription(w: GalleryWork): string {
  const prov = artProvenance(w.kind, w.context, w.honoree);
  switch (artForm(w.kind, w.motif)) {
    case 'orb':      return `A rounded orb, shaped by ${w.maker} ${prov}.`;
    case 'spire':    return `A slender spire, raised by ${w.maker} ${prov}.`;
    case 'arch':     return `A little arch to pass beneath, built by ${w.maker} ${prov}.`;
    case 'cairn':    return `A stack of balanced stones, piled by ${w.maker} ${prov}.`;
    case 'memorial': return `A carved stone, set by ${w.maker} ${prov}.`;
    case 'paint_diamonds':
      return `Pigment pressed into the floor in a scatter of diamonds, by ${w.maker} ${prov}.`;
    case 'paint_meander':
      return `Pigment pressed into the floor in winding trails, by ${w.maker} ${prov}.`;
    case 'lost':
      return w.kind === 'painting'
        ? `A floor painting whose pattern is lost to time, made by ${w.maker} ${prov}.`
        : `A sculpture whose form is lost to time, shaped by ${w.maker} ${prov}.`;
  }
  switch (w.kind) {
    case 'petal_hat':    return `A petal hat, woven by ${w.maker} ${prov}.`;
    case 'seed_pendant': return `A seed pendant, strung by ${w.maker} ${prov}.`;
    case 'grass_band':   return `A grass band, plaited by ${w.maker} ${prov}.`;
    default:             return `A ${artKindLabel(w.kind)}, made by ${w.maker} ${prov}.`;
  }
}

export interface GalleryWork {
  kind: string;
  maker: string;
  makerId: number;
  context: string;
  honoree?: string;
  motif?: number;   // absent on works that predate the field
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
        motif: typeof data.motif === 'number' ? data.motif : undefined,
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
