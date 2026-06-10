import { useState, useEffect } from 'react';
import { Card } from './Card';
import { fetchEvents } from '../api/client';
import { useColony } from '../state/colony';
import { nameFromId } from '../data/plantNames';
import {
  computeDigest, digestLines, awayLabel,
  getLastVisitUnix, markVisitNow, shouldShowDigest,
  type Digest,
} from '../data/digest';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';

/* "While you were away" — narrative summary card shown at the top of Home
 * when the user returns after 30+ minutes and something happened. */

export function DigestCard({ palette }: {
  palette: { cardBg: string; text: string; dimText: string };
}) {
  const { snapshot, colonyId } = useColony();
  const [digest, setDigest] = useState<Digest | null>(null);
  const [dismissed, setDismissed] = useState(false);

  useEffect(() => {
    if (!colonyId) return;
    if (!shouldShowDigest()) {
      // First visit or quick return — just refresh the marker
      markVisitNow();
      return;
    }
    const since = getLastVisitUnix();
    let cancelled = false;
    (async () => {
      const result = await fetchEvents(colonyId, since, 1000);
      if (cancelled || !result) return;
      const nameFor = (id: number): string => {
        const fromRoster = snapshot?.lilguys?.find(l => l.id === id)?.name;
        return fromRoster || nameFromId(id);
      };
      const d = computeDigest(result.data.results, since, nameFor);
      if (!d.isEmpty) setDigest(d);
      else markVisitNow(); // nothing happened — reset quietly
    })();
    return () => { cancelled = true; };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [colonyId]);

  if (!digest || dismissed) return null;

  const lines = digestLines(digest);

  return (
    <Card style={{
      background: palette.cardBg,
      borderLeft: `3px solid ${HIVE.accent}`,
    }}>
      <div style={{
        display: 'flex', justifyContent: 'space-between',
        alignItems: 'baseline', marginBottom: 8,
      }}>
        <div style={{
          fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText,
          textTransform: 'uppercase', letterSpacing: 1,
        }}>
          While you were away
        </div>
        <div style={{ fontSize: SIZES.xs, color: palette.dimText }}>
          {awayLabel(digest.awayHours)}
        </div>
      </div>

      {lines.map((line, i) => (
        <div key={i} style={{
          display: 'flex', gap: 8, alignItems: 'flex-start',
          marginBottom: i === lines.length - 1 ? 0 : 6,
        }}>
          <span style={{ fontSize: 14, lineHeight: '20px' }}>{line.icon}</span>
          <span style={{ fontSize: SIZES.sm, color: palette.text, lineHeight: '20px' }}>
            {line.text}
          </span>
        </div>
      ))}

      <button
        onClick={() => { markVisitNow(); setDismissed(true); }}
        style={{
          marginTop: 10, background: 'none',
          border: `1px solid ${HIVE.sand}`, borderRadius: 16,
          padding: '4px 14px', fontSize: SIZES.xs,
          color: palette.dimText, cursor: 'pointer',
        }}
      >
        Caught up
      </button>
    </Card>
  );
}
