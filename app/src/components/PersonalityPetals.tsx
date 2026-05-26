import type { Personality } from '../api/types';
import { PERSONALITY_DIMS } from '../data/personality';
import { HIVE } from '../theme/palette';

interface Props {
  personality: Personality;
  size?: number;
}

export function PersonalityPetals({ personality, size = 140 }: Props) {
  const cx = size / 2;
  const cy = size / 2;
  const maxR = size / 2 - 16;
  const dims = PERSONALITY_DIMS;
  const n = dims.length;

  // Build polygon points
  const points = dims.map((d, i) => {
    const angle = (Math.PI * 2 * i) / n - Math.PI / 2;
    const val = personality[d.key as keyof Personality] ?? 0.5;
    const r = maxR * Math.max(0.08, val);
    return {
      x: cx + r * Math.cos(angle),
      y: cy + r * Math.sin(angle),
      label: d.label,
      val,
      labelX: cx + (maxR + 12) * Math.cos(angle),
      labelY: cy + (maxR + 12) * Math.sin(angle),
    };
  });

  const polyStr = points.map(p => `${p.x},${p.y}`).join(' ');

  // Background rings
  const rings = [0.25, 0.5, 0.75, 1.0];

  return (
    <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
      {/* Guide rings */}
      {rings.map(r => (
        <circle
          key={r}
          cx={cx} cy={cy}
          r={maxR * r}
          fill="none"
          stroke={HIVE.sand}
          strokeWidth={0.5}
          opacity={0.4}
        />
      ))}

      {/* Axis lines */}
      {dims.map((_, i) => {
        const angle = (Math.PI * 2 * i) / n - Math.PI / 2;
        return (
          <line
            key={i}
            x1={cx} y1={cy}
            x2={cx + maxR * Math.cos(angle)}
            y2={cy + maxR * Math.sin(angle)}
            stroke={HIVE.sand}
            strokeWidth={0.5}
            opacity={0.3}
          />
        );
      })}

      {/* Petal shape */}
      <polygon
        points={polyStr}
        fill={HIVE.accent}
        fillOpacity={0.25}
        stroke={HIVE.accent}
        strokeWidth={1.5}
      />

      {/* Vertices */}
      {points.map((p, i) => (
        <circle key={i} cx={p.x} cy={p.y} r={2.5} fill={HIVE.accent} />
      ))}

      {/* Labels */}
      {points.map((p, i) => (
        <text
          key={i}
          x={p.labelX}
          y={p.labelY}
          textAnchor="middle"
          dominantBaseline="central"
          fontSize={8}
          fill={HIVE.dimText}
        >
          {p.label}
        </text>
      ))}
    </svg>
  );
}
