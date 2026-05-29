import { useMemo } from 'react';

// Firmware tint algorithm (from renderer.cpp):
// tint_r = ((tint_seed & 0x07) - 3) * 2        → -6..+8  (on 5-bit R, 0-31)
// tint_g = ((tint_seed >> 3) & 0x07) - 3        → -3..+4  (on 6-bit G, 0-63)
// tint_b = (((tint_seed >> 6) & 0x03) - 1) * 2  → -2..+2  (on 5-bit B, 0-31)
// Applied as direct per-channel offsets on RGB565 pixel values.

// SVG filter ID prefix (unique per seed to avoid collisions)
let _filterCounter = 0;

function useTintFilter(seed: number): { filterId: string; filterSvg: JSX.Element | null } {
  return useMemo(() => {
    if (seed === 0) return { filterId: '', filterSvg: null };

    const r = ((seed & 0x07) - 3) * 2;
    const g = ((seed >> 3) & 0x07) - 3;
    const b = (((seed >> 6) & 0x03) - 1) * 2;

    if (r === 0 && g === 0 && b === 0) return { filterId: '', filterSvg: null };

    // Map firmware RGB565 offsets to 8-bit: R/B *8, G *4
    const rOff = (r * 8) / 255;
    const gOff = (g * 4) / 255;
    const bOff = (b * 8) / 255;

    const id = `tint-${seed}-${++_filterCounter}`;
    const svg = (
      <svg width={0} height={0} style={{ position: 'absolute' }}>
        <defs>
          <filter id={id} colorInterpolationFilters="sRGB">
            <feComponentTransfer>
              <feFuncR type="linear" slope={1} intercept={rOff} />
              <feFuncG type="linear" slope={1} intercept={gOff} />
              <feFuncB type="linear" slope={1} intercept={bOff} />
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
  displaySize?: number; // base display size in px (at scale 1.0)
  palette: { dimText: string };
}

export function ConkerSprite({ scaleFactor = SCALE_MEAN, tintSeed = 0, displaySize = 80, palette }: Props) {
  const relativeScale = scaleFactor / SCALE_MEAN;
  const spriteSize = displaySize * relativeScale;
  const { filterId, filterSvg } = useTintFilter(tintSeed);

  // Height label: mean scale_factor 3.2 = 1cm
  const heightCm = (scaleFactor / SCALE_MEAN).toFixed(1);

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
          {heightCm} cm
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
