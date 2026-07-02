import { useMemo } from 'react';
import { growthFactor } from '../data/growth';

// Per-conker colour (mirrors renderer.cpp `_draw_sprite_scaled_tinted`):
// every conker gets its own vivid hue so the colony is easy to tell apart; what's
// distributed like the size bell curve is how far a conker may stray from the warm
// band. Common = a warm hue at strong saturation; a rare roll widens the band so
// the occasional outlier lands on a vivid off-hue.
//
// The firmware recolours each pixel onto a luma-scaled target ramp; SVG filters
// can't do that per-pixel lerp, so we approximate the read with saturate +
// hueRotate driven by the identical seed→(hue, rarity) derivation.

// The bare sprite's brown sits near this hue (degrees); we rotate onto the target.
const SPRITE_BASE_HUE = 32;

// SVG filter ID prefix (unique per seed to avoid collisions)
let _filterCounter = 0;

export function useTintFilter(seed: number): { filterId: string; filterSvg: JSX.Element | null } {
  return useMemo(() => {
    if (seed === 0) return { filterId: '', filterSvg: null };

    // Same hash + derivation as the firmware (Knuth multiplicative).
    const hs = (seed * 2654435761) >>> 0;
    const ur = (hs & 0xffff) / 65535;
    const uh = ((hs >>> 16) & 0xffff) / 65535;
    // Decorrelated third roll for per-conker lightness (matches firmware h2/ul).
    const h2 = (hs ^ (hs >>> 13)) >>> 0;
    const ul = ((h2 >>> 5) & 0xff) / 255;

    const rare = ur ** 6; // straying far is rare
    const spread = 40 + 240 * rare;
    let hue = 28 + (uh * 2 - 1) * spread; // anchor on orange (28°)
    hue = ((hue % 360) + 360) % 360;

    const rotateDeg = ((hue - SPRITE_BASE_HUE + 540) % 360) - 180;
    const saturate = 1.5 + 0.6 * rare;
    // Per-conker brightness; rare conkers that also roll bright get an extra lift to pop.
    const bright = 1.0 + 0.35 * ul + 0.55 * rare * ul;

    const id = `tint-${seed}-${++_filterCounter}`;
    const svg = (
      <svg width={0} height={0} style={{ position: 'absolute' }}>
        <defs>
          <filter id={id} colorInterpolationFilters="sRGB">
            <feColorMatrix type="saturate" values={`${saturate}`} />
            <feColorMatrix type="hueRotate" values={`${rotateDeg}`} />
            <feComponentTransfer>
              <feFuncR type="linear" slope={bright} />
              <feFuncG type="linear" slope={bright} />
              <feFuncB type="linear" slope={bright} />
            </feComponentTransfer>
          </filter>
        </defs>
      </svg>
    );
    return { filterId: id, filterSvg: svg };
  }, [seed]);
}

// Scale factor: firmware uses mean=3.2, range 2.2-4.2
// Normalize so 3.2 = 1.0x display scale
const SCALE_MEAN = 3.2;

interface Props {
  scaleFactor?: number;
  tintSeed?: number;
  ageDays?: number;     // growth: under a day old renders smaller (mirrors glass)
  displaySize?: number; // base display size in px (at scale 1.0)
  palette: { dimText: string };
}

export function ConkerSprite({ scaleFactor = SCALE_MEAN, tintSeed = 0, ageDays, displaySize = 80, palette }: Props) {
  const grown = growthFactor(ageDays);
  const relativeScale = (scaleFactor / SCALE_MEAN) * grown;
  const spriteSize = displaySize * relativeScale;
  const { filterId, filterSvg } = useTintFilter(tintSeed);

  // Height label: mean scale_factor 3.2 = 1cm, scaled down while growing
  const heightCm = relativeScale.toFixed(1);
  const stillGrowing = grown < 1;

  // Scale bar: shows 1cm reference
  const refBarHeight = displaySize; // 1cm = base display size

  return (
    <div style={{ display: 'flex', alignItems: 'flex-end', justifyContent: 'center', gap: 16, padding: '12px 0' }}>
      {filterSvg}
      {/* Conker sprite */}
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
        <img
          src="/app/conker.png"
          alt="Conker"
          style={{
            width: spriteSize,
            height: spriteSize,
            imageRendering: 'pixelated',
            filter: filterId ? `url(#${filterId})` : undefined,
          }}
        />
        <div style={{ fontSize: 11, color: palette.dimText, marginTop: 4 }}>
          {heightCm} cm{stillGrowing ? ' · still growing' : ''}
        </div>
      </div>

      {/* Scale reference */}
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'flex-end' }}>
        <div style={{
          width: 2,
          height: refBarHeight,
          background: palette.dimText,
          opacity: 0.3,
          position: 'relative',
        }}>
          {/* Top tick */}
          <div style={{ position: 'absolute', top: 0, left: -3, width: 8, height: 1, background: palette.dimText, opacity: 0.5 }} />
          {/* Bottom tick */}
          <div style={{ position: 'absolute', bottom: 0, left: -3, width: 8, height: 1, background: palette.dimText, opacity: 0.5 }} />
        </div>
        <div style={{ fontSize: 9, color: palette.dimText, opacity: 0.5, marginTop: 2 }}>
          1 cm
        </div>
      </div>
    </div>
  );
}
