import type { Module } from '../api/types';
import { HIVE } from '../theme/palette';

interface Props {
  modules: Module[];
  size?: number;
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
  if (!queen) return modules.map((m, i) => ({ module: m, x: i * 60, y: 0 }));

  // Place queen at center
  placed.set(queen.id, { x: 0, y: 0 });

  // BFS placement from faces
  const queue = [queen];
  const step = 50;
  const faceOffsets: Record<string, { dx: number; dy: number }> = {
    north: { dx: 0, dy: -step },
    south: { dx: 0, dy: step },
    west: { dx: -step, dy: 0 },
    east: { dx: step, dy: 0 },
  };

  while (queue.length > 0) {
    const current = queue.shift()!;
    const pos = placed.get(current.id)!;

    for (const [face, neighborId] of Object.entries(current.faces)) {
      if (!neighborId || placed.has(neighborId)) continue;
      const offset = faceOffsets[face];
      if (!offset) continue;
      placed.set(neighborId, { x: pos.x + offset.dx, y: pos.y + offset.dy });
      const neighbor = modules.find(m => m.id === neighborId);
      if (neighbor) queue.push(neighbor);
    }
  }

  // Include any unplaced modules
  let unplacedX = 0;
  for (const m of modules) {
    if (!placed.has(m.id)) {
      placed.set(m.id, { x: unplacedX * 60, y: 80 });
      unplacedX++;
    }
  }

  return modules.map(m => ({ module: m, ...placed.get(m.id)! }));
}

export function TopologyMiniMap({ modules, size = 120 }: Props) {
  const positions = layoutModules(modules);
  if (positions.length === 0) return null;

  // Normalize to fit in viewBox
  const xs = positions.map(p => p.x);
  const ys = positions.map(p => p.y);
  const minX = Math.min(...xs) - 20;
  const minY = Math.min(...ys) - 20;
  const maxX = Math.max(...xs) + 20;
  const maxY = Math.max(...ys) + 20;
  const w = Math.max(maxX - minX, 40);
  const h = Math.max(maxY - minY, 40);

  return (
    <svg width={size} height={size} viewBox={`${minX} ${minY} ${w} ${h}`}>
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
              strokeWidth={2}
            />
          );
        })
      )}

      {/* Module nodes */}
      {positions.map(p => (
        <g key={p.module.id}>
          <rect
            x={p.x - 12} y={p.y - 8}
            width={24} height={16}
            rx={3}
            fill={p.module.role === 'queen' ? HIVE.accent : HIVE.bark}
            opacity={p.module.online ? 1 : 0.4}
          />
          <text
            x={p.x} y={p.y + 1}
            textAnchor="middle"
            dominantBaseline="central"
            fontSize={6}
            fontWeight={600}
            fill={HIVE.white}
          >
            {p.module.role === 'queen' ? 'Q' : 'S'}
          </text>
        </g>
      ))}
    </svg>
  );
}
