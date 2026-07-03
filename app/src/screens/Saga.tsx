import { useEffect, useMemo, useState } from 'react';
import { useColony } from '../state/colony';
import { Card } from '../components/Card';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { fetchEvents } from '../api/client';
import { composeSaga } from '../data/saga';
import type { ColonyEvent } from '../api/types';

// The Saga — the colony's history as chapters of prose, composed from
// the event log. Newest chapter first; the founding at the bottom,
// where beginnings belong.
export function Saga({ onBack }: { onBack: () => void }) {
  const { colonyId, events: liveEvents, snapshot } = useColony();
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

  const chapters = useMemo(
    () => composeSaga(history ?? liveEvents, snapshot?.founded_unix),
    [history, liveEvents, snapshot]);

  const dateStr = (unix: number) =>
    new Date(unix * 1000).toLocaleDateString(undefined, { day: 'numeric', month: 'long' });

  return (
    <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <div style={{ padding: '16px 0 4px' }}>
        <button onClick={onBack} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: SIZES.base, color: HIVE.accent, padding: 0 }}>
          &larr; Back
        </button>
      </div>
      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: palette.text, margin: '0 0 2px' }}>
        The Saga of {colonyId}
      </h1>
      <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginBottom: 14, fontStyle: 'italic' }}>
        As the chronicle tells it.
      </div>

      {chapters.length === 0 ? (
        <div style={{ color: palette.dimText, fontSize: SIZES.base, textAlign: 'center', padding: 32 }}>
          The story hasn’t started yet.
        </div>
      ) : (
        chapters.map((ch, i) => (
          <Card key={i} style={{ background: palette.cardBg, padding: 16 }}>
            <div style={{ fontSize: SIZES.base, fontWeight: 700, color: palette.text }}>
              {ch.title}
            </div>
            <div style={{ fontSize: SIZES.xs, color: palette.dimText, margin: '2px 0 10px' }}>
              from {dateStr(ch.startUnix)}
            </div>
            {ch.paragraphs.map((p, j) => (
              <p key={j} style={{
                fontSize: SIZES.sm, color: palette.text,
                lineHeight: 1.55, margin: '0 0 8px',
              }}>
                {p}
              </p>
            ))}
          </Card>
        ))
      )}

      <div style={{ fontSize: SIZES.xs, color: palette.dimText, textAlign: 'center', padding: '14px 24px 0', fontStyle: 'italic' }}>
        New chapters write themselves as the weeks turn.
      </div>
    </div>
  );
}
