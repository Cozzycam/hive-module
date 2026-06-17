import type { Bond } from '../api/types';
import { HIVE } from '../theme/palette';

interface Props {
  name: string;
  bonds: Bond[];
  size?: number;
}

export function BondsWeb({ name, bonds, size = 200 }: Props) {
  if (bonds.length === 0) {
    return (
      <div style={{ color: HIVE.dimText, fontSize: 13, textAlign: 'center', padding: 16 }}>
        No bonds yet
      </div>
    );
  }

  const cx = size / 2;
  const cy = size / 2;
  const outerR = size / 2 - 30;

  const nodes = bonds.map((b, i) => {
    const angle = (Math.PI * 2 * i) / bonds.length - Math.PI / 2;
    return {
      x: cx + outerR * Math.cos(angle),
      y: cy + outerR * Math.sin(angle),
      bond: b,
    };
  });

  return (
    <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
      {/* Lines from center to each bonded lil guy. Mutual (best friends) draw
          solid + bold; a one-way soft spot is faint and dashed. */}
      {nodes.map((n, i) => (
        <line
          key={i}
          x1={cx} y1={cy}
          x2={n.x} y2={n.y}
          stroke={HIVE.accent}
          strokeWidth={Math.max(1, n.bond.strength * 3)}
          strokeDasharray={n.bond.reciprocated ? undefined : '3 3'}
          opacity={n.bond.reciprocated ? 0.4 + n.bond.strength * 0.5 : 0.2 + n.bond.strength * 0.3}
        />
      ))}

      {/* Center node (this lil guy) */}
      <circle cx={cx} cy={cy} r={6} fill={HIVE.soot} />
      <text
        x={cx} y={cy - 10}
        textAnchor="middle"
        fontSize={10}
        fontWeight={600}
        fill={HIVE.ink}
      >
        {name}
      </text>

      {/* Bonded nodes */}
      {nodes.map((n, i) => (
        <g key={i}>
          <circle
            cx={n.x} cy={n.y}
            r={4 + n.bond.strength * 2}
            fill={HIVE.accent}
            opacity={0.6 + n.bond.strength * 0.4}
          />
          <text
            x={n.x}
            y={n.y + (n.y > cy ? 14 : -10)}
            textAnchor="middle"
            fontSize={9}
            fill={HIVE.dimText}
          >
            {n.bond.reciprocated ? `⭐ ${n.bond.to.name}` : n.bond.to.name}
          </text>
        </g>
      ))}
    </svg>
  );
}
