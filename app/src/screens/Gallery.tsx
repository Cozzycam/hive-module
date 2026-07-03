import { useEffect, useMemo, useState } from 'react';
import { useColony } from '../state/colony';
import { Card } from '../components/Card';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { fetchEvents } from '../api/client';
import { buildGallery, artKindEmoji, artKindLabel, artProvenance } from '../data/artworks';
import type { ColonyEvent } from '../api/types';

// The Gallery — every work the colony's makers have left in the world,
// with its provenance. Weathered pieces stay listed, dimmed: even what
// crumbles happened.
export function Gallery({ onBack }: { onBack: () => void }) {
  const { colonyId, events: liveEvents } = useColony();
  const palette = TOD_PALETTES.day;
  const [history, setHistory] = useState<ColonyEvent[] | null>(null);

  useEffect(() => {
    let cancelled = false;
    if (!colonyId) return;
    fetchEvents(colonyId, 0, 1000).then(res => {
      if (!cancelled && res?.data?.results) setHistory(res.data.results);
    });
    return () => { cancelled = true; };
  }, [colonyId]);

  const works = useMemo(
    () => buildGallery(history ?? liveEvents),
    [history, liveEvents]);

  const standing = works.filter(w => !w.weathered).length;

  const dateStr = (unix: number) =>
    new Date(unix * 1000).toLocaleDateString(undefined, { month: 'short', day: 'numeric' });

  return (
    <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <div style={{ padding: '16px 0 4px' }}>
        <button onClick={onBack} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: SIZES.base, color: HIVE.accent, padding: 0 }}>
          &larr; Back
        </button>
      </div>
      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: palette.text, margin: '0 0 2px' }}>
        Gallery
      </h1>
      <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginBottom: 14 }}>
        {works.length === 0
          ? 'Nothing made yet — the muse visits when the pantry is deep.'
          : `${standing} work${standing === 1 ? '' : 's'} standing · ${works.length} ever made.`}
      </div>

      {works.map((w, i) => (
        <Card key={i} style={{ background: palette.cardBg, opacity: w.weathered ? 0.55 : 1 }}>
          <div style={{ display: 'flex', gap: 12, alignItems: 'flex-start' }}>
            <div style={{ fontSize: 30, lineHeight: '36px', minWidth: 40, textAlign: 'center' }}>
              {artKindEmoji(w.kind)}
            </div>
            <div style={{ flex: 1 }}>
              <div style={{ fontSize: SIZES.base, fontWeight: 600, color: palette.text }}>
                {w.maker}{'’'}s {artKindLabel(w.kind)}
                {w.weathered && (
                  <span style={{ fontWeight: 400, fontSize: SIZES.xs, color: palette.dimText }}>
                    {' '}· crumbled
                  </span>
                )}
              </div>
              <div style={{ fontSize: SIZES.sm, color: palette.text, marginTop: 2 }}>
                Made {artProvenance(w.kind, w.context, w.honoree)}.
              </div>
              <div style={{ fontSize: SIZES.xs, color: palette.dimText, marginTop: 4 }}>
                {dateStr(w.unix)}
              </div>
            </div>
          </div>
        </Card>
      ))}

      <div style={{ fontSize: SIZES.xs, color: palette.dimText, textAlign: 'center', padding: '14px 24px 0', fontStyle: 'italic' }}>
        Works stand in the chamber in their maker{'’'}s colours. When the
        chamber fills, the oldest weather away.
      </div>
    </div>
  );
}
