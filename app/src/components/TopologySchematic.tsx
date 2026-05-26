import type { Module } from '../api/types';
import { HIVE } from '../theme/palette';

interface Props {
  modules: Module[];
  onModuleClick?: (module: Module) => void;
  showPheromones?: boolean;
}

interface ModulePos {
  module: Module;
  x: number;
  y: number;
}

function layoutModules(modules: Module[]): ModulePos[] {
  if (modules.length === 0) return [];

  const placed = new Map<string, { x: number; y: number }>();
  const queen = modules.find(m => m.role === 'queen');
  if (!queen) return modules.map((m, i) => ({ module: m, x: i * 120 + 60, y: 60 }));

  placed.set(queen.id, { x: 200, y: 100 });
  const step = 120;
  const faceOffsets: Record<string, { dx: number; dy: number }> = {
    north: { dx: 0, dy: -step },
    south: { dx: 0, dy: step },
    west: { dx: -step, dy: 0 },
    east: { dx: step, dy: 0 },
  };

  const queue = [queen];
  while (queue.length > 0) {
    const current = queue.shift()!;
    const pos = placed.get(current.id)!;
    for (const [face, nId] of Object.entries(current.faces)) {
      if (!nId || placed.has(nId)) continue;
      const offset = faceOffsets[face];
      if (!offset) continue;
      placed.set(nId, { x: pos.x + offset.dx, y: pos.y + offset.dy });
      const neighbor = modules.find(m => m.id === nId);
      if (neighbor) queue.push(neighbor);
    }
  }

  return modules.map(m => {
    const pos = placed.get(m.id) || { x: 60, y: 60 };
    return { module: m, ...pos };
  });
}

const FACE_LABELS: Record<string, string> = { north: 'N', south: 'S', west: 'W', east: 'E' };
const FACE_OFFSETS: Record<string, { dx: number; dy: number }> = {
  north: { dx: 0, dy: -28 },
  south: { dx: 0, dy: 28 },
  west: { dx: -36, dy: 0 },
  east: { dx: 36, dy: 0 },
};

export function TopologySchematic({ modules, onModuleClick, showPheromones }: Props) {
  const positions = layoutModules(modules);
  if (positions.length === 0) return null;

  const xs = positions.map(p => p.x);
  const ys = positions.map(p => p.y);
  const padding = 60;
  const minX = Math.min(...xs) - padding;
  const minY = Math.min(...ys) - padding;
  const w = Math.max(...xs) - minX + padding;
  const h = Math.max(...ys) - minY + padding;

  return (
    <svg
      width="100%"
      viewBox={`${minX} ${minY} ${w} ${h}`}
      style={{ maxHeight: 300 }}
    >
      {/* Connection lines */}
      {positions.map(p =>
        Object.entries(p.module.faces).map(([face, nId]) => {
          if (!nId) return null;
          const neighbor = positions.find(n => n.module.id === nId);
          if (!neighbor) return null;
          return (
            <line
              key={`${p.module.id}-${face}`}
              x1={p.x} y1={p.y}
              x2={neighbor.x} y2={neighbor.y}
              stroke={HIVE.sand}
              strokeWidth={3}
              strokeDasharray={showPheromones ? undefined : '6 3'}
            />
          );
        })
      )}

      {/* Pheromone blooms on connections */}
      {showPheromones && positions.map(p =>
        Object.entries(p.module.faces).map(([face, nId]) => {
          if (!nId) return null;
          const neighbor = positions.find(n => n.module.id === nId);
          if (!neighbor) return null;
          const mx = (p.x + neighbor.x) / 2;
          const my = (p.y + neighbor.y) / 2;
          return (
            <circle
              key={`ph-${p.module.id}-${face}`}
              cx={mx} cy={my} r={8}
              fill={HIVE.leafGreen}
              opacity={0.25}
            />
          );
        })
      )}

      {/* Module boxes */}
      {positions.map(p => (
        <g
          key={p.module.id}
          onClick={() => onModuleClick?.(p.module)}
          style={{ cursor: onModuleClick ? 'pointer' : undefined }}
        >
          <rect
            x={p.x - 30} y={p.y - 20}
            width={60} height={40}
            rx={6}
            fill={p.module.role === 'queen' ? HIVE.accent : HIVE.bark}
            opacity={p.module.online ? 1 : 0.4}
            stroke={HIVE.soot}
            strokeWidth={1}
          />
          <text
            x={p.x} y={p.y - 4}
            textAnchor="middle"
            fontSize={10}
            fontWeight={700}
            fill={HIVE.white}
          >
            {p.module.role === 'queen' ? 'Queen' : p.module.id}
          </text>
          <text
            x={p.x} y={p.y + 8}
            textAnchor="middle"
            fontSize={7}
            fill={HIVE.white}
            opacity={0.7}
          >
            {p.module.id}
          </text>

          {/* Face labels */}
          {Object.entries(p.module.faces).map(([face, nId]) => {
            if (!nId) return null;
            const off = FACE_OFFSETS[face];
            if (!off) return null;
            return (
              <text
                key={face}
                x={p.x + off.dx}
                y={p.y + off.dy}
                textAnchor="middle"
                dominantBaseline="central"
                fontSize={7}
                fill={HIVE.dimText}
              >
                {FACE_LABELS[face]}
              </text>
            );
          })}
        </g>
      ))}
    </svg>
  );
}
