import { useMemo } from 'react';

// Firmware tint algorithm (from renderer.cpp):
// tint_r = ((tint_seed & 0x07) - 3) * 2        → -6..+8
// tint_g = ((tint_seed >> 3) & 0x07) - 3        → -3..+4
// tint_b = (((tint_seed >> 6) & 0x03) - 1) * 2  → -2..+2
// Full seed range 1-255 used for colour; ageing grey handled separately.

function tintToCSS(seed: number): string {
  if (seed === 0) return 'none';
  const r = ((seed & 0x07) - 3) * 2;
  const g = ((seed >> 3) & 0x07) - 3;
  const b = (((seed >> 6) & 0x03) - 1) * 2;
  // Map RGB565 channel offsets to CSS hue-rotate + saturate.
  // R vs B balance drives hue; G drives saturation.
  const hueShift = (r - b) * 5;  // ±50 degrees
  const satBoost = 1.0 + g * 0.08;
  return `hue-rotate(${hueShift}deg) saturate(${satBoost})`;
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
  const filter = useMemo(() => tintToCSS(tintSeed), [tintSeed]);

  // Height label: mean scale_factor 3.2 = 1cm
  const heightCm = (scaleFactor / SCALE_MEAN).toFixed(1);

  // Scale bar: shows 1cm reference
  const refBarHeight = displaySize; // 1cm = base display size
  const conkerBarHeight = spriteSize;

  return (
    <div style={{ display: 'flex', alignItems: 'flex-end', justifyContent: 'center', gap: 16, padding: '12px 0' }}>
      {/* Conker sprite */}
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center' }}>
        <img
          src="/app/conker.png"
          alt="Conker"
          style={{
            width: spriteSize,
            height: spriteSize,
            imageRendering: 'pixelated',
            filter,
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
