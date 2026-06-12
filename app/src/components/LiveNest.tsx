import { useState } from 'react';
import { useTintFilter } from './ConkerSprite';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import type { ColonySnapshot, LilGuySummary } from '../api/types';

// Live nest view (Amber: "I would like to see the wee guys move around in
// the app"). Positions arrive with each snapshot poll (~15s); sprites drift
// to their new spot over several seconds so the nest always looks alive.

const SCALE_MEAN = 3.2;
const BASE_SPRITE_PX = 18;

function NestConker({
  lg, gridW, gridH, selected, onTap, onOpen, palette,
}: {
  lg: LilGuySummary;
  gridW: number;
  gridH: number;
  selected: boolean;
  onTap: () => void;
  onOpen: () => void;
  palette: { text: string; cardBg: string };
}) {
  const { filterId, filterSvg } = useTintFilter(lg.tint_seed ?? 0);
  const size = BASE_SPRITE_PX * ((lg.scale_factor ?? SCALE_MEAN) / SCALE_MEAN);

  return (
    <div
      onClick={(e) => { e.stopPropagation(); onTap(); }}
      style={{
        position: 'absolute',
        left: `${(((lg.x ?? 0) + 0.5) / gridW) * 100}%`,
        top: `${(((lg.y ?? 0) + 0.5) / gridH) * 100}%`,
        transform: 'translate(-50%, -50%)',
        transition: 'left 8s linear, top 8s linear',
        cursor: 'pointer',
        zIndex: selected ? 3 : 2,
        // Generous tap target around a small sprite
        padding: 6,
        margin: -6,
      }}
    >
      {filterSvg}
      <img
        src="/app/conker.png"
        alt={lg.name}
        style={{
          width: size,
          height: size,
          display: 'block',
          imageRendering: 'pixelated',
          filter: filterId ? `url(#${filterId})` : undefined,
          animation: `nest-bob 2.4s ease-in-out ${(lg.id % 8) * 0.3}s infinite`,
        }}
      />
      {selected && (
        <div
          onClick={(e) => { e.stopPropagation(); onOpen(); }}
          style={{
            position: 'absolute',
            bottom: '100%',
            left: '50%',
            transform: 'translateX(-50%)',
            marginBottom: 2,
            padding: '3px 8px',
            background: palette.cardBg,
            color: palette.text,
            fontSize: SIZES.xs,
            fontWeight: 600,
            borderRadius: 8,
            whiteSpace: 'nowrap',
            boxShadow: '0 1px 4px rgba(0,0,0,0.25)',
          }}
        >
          {lg.name} &rsaquo;
        </div>
      )}
    </div>
  );
}

export function LiveNest({
  snapshot, palette, onOpenCharacter,
}: {
  snapshot: ColonySnapshot;
  palette: { text: string; dimText: string; cardBg: string };
  onOpenCharacter: (id: number) => void;
}) {
  const [selectedId, setSelectedId] = useState<number | null>(null);

  const grid = snapshot.grid;
  const positioned = (snapshot.lilguys ?? []).filter(
    l => typeof l.x === 'number' && typeof l.y === 'number',
  );
  // Older firmware doesn't send positions — quietly show nothing
  if (!grid || positioned.length === 0) return null;

  const nightFactor = snapshot.world?.tod?.night_factor ?? 0;
  const floorTint = snapshot.modules?.[0]?.tint;
  const floor = floorTint
    ? `color-mix(in srgb, ${floorTint} 30%, ${HIVE.sand})`
    : HIVE.sand;
  const queen = snapshot.queen_pos;

  return (
    <div style={{ margin: '12px 0' }}>
      <style>{`@keyframes nest-bob {
        0%, 100% { transform: translateY(0); }
        50% { transform: translateY(-1.5px); }
      }`}</style>
      <div style={{
        fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText,
        textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8,
      }}>
        The Nest &middot; live
      </div>
      <div
        onClick={() => setSelectedId(null)}
        style={{
          position: 'relative',
          width: '100%',
          aspectRatio: `${grid.w} / ${grid.h}`,
          background: floor,
          borderRadius: 16,
          border: `2px solid ${HIVE.bark}`,
          overflow: 'hidden',
        }}
      >
        {/* Queen — her actual sprite, extracted from the firmware */}
        {queen && (
          <div style={{
            position: 'absolute',
            left: `${((queen.x + 0.5) / grid.w) * 100}%`,
            top: `${((queen.y + 0.5) / grid.h) * 100}%`,
            transform: 'translate(-50%, -50%)',
            transition: 'left 8s linear, top 8s linear',
            zIndex: 1,
          }}>
            <img
              src="/app/queen.png"
              alt="Queen"
              style={{ width: 36, height: 36, display: 'block', imageRendering: 'pixelated' }}
            />
          </div>
        )}

        {positioned.map(lg => (
          <NestConker
            key={lg.id}
            lg={lg}
            gridW={grid.w}
            gridH={grid.h}
            selected={selectedId === lg.id}
            onTap={() => setSelectedId(selectedId === lg.id ? null : lg.id)}
            onOpen={() => onOpenCharacter(lg.id)}
            palette={palette}
          />
        ))}

        {/* Night falls over the nest just like on the module — gently;
            you should still see everyone (Campbell: was too dark) */}
        {nightFactor > 0 && (
          <div style={{
            position: 'absolute', inset: 0,
            background: `rgba(30, 32, 58, ${(nightFactor * 0.22).toFixed(2)})`,
            pointerEvents: 'none',
            zIndex: 4,
          }} />
        )}
      </div>
    </div>
  );
}
