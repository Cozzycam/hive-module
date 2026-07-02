import { useEffect, useMemo, useState } from 'react';
import { useColony } from '../state/colony';
import { Card } from '../components/Card';
import { nameFromId } from '../data/plantNames';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { fetchEvents } from '../api/client';
import type { ColonyEvent } from '../api/types';

// The Field Guide — every visiting critter the colony has ever spotted.
// (Amber: "you could have a page on the app for found creatures" — and
// "Let me see the bugs!!!", three times.)
// Undiscovered visitors show as silhouettes with a hint of how to meet them.
const CRITTERS: {
  kind: string; emoji: string; name: string; blurb: string; hint: string;
}[] = [
  {
    kind: 'butterfly', emoji: '\u{1F98B}', name: 'Butterfly',
    blurb: 'Flutters in on warm, clear days and drifts among the wee guys.',
    hint: 'Visits by day, in fair weather. Gardens see the most.',
  },
  {
    kind: 'beetle', emoji: '\u{1FAB2}', name: 'Beetle',
    blurb: 'Trundles through on important beetle business.',
    hint: 'Visits by day, in fair weather.',
  },
  {
    kind: 'worm', emoji: '\u{1FAB1}', name: 'Worm',
    blurb: 'Surfaces for a wander between the grains.',
    hint: 'Visits by day — likes the ground soft.',
  },
  {
    kind: 'firefly', emoji: '\u{2728}', name: 'Firefly',
    blurb: 'Glimmers over the chamber after dusk. The brave give chase — '
         + 'whoever catches the most holds the Bug Hunter title.',
    hint: 'Only after dark. Watch for the chase!',
  },
];

interface KindStats {
  count: number;
  firstUnix: number;
  firstFinder: string;
  lastUnix: number;
}

export function FieldGuide({ onBack }: { onBack: () => void }) {
  const { colonyId, events: liveEvents, snapshot } = useColony();
  const palette = TOD_PALETTES.day;
  const [history, setHistory] = useState<ColonyEvent[] | null>(null);

  // The poller only holds a recent window — pull a deeper slice for the
  // all-time collection (the server caps at 1000 newest).
  useEffect(() => {
    let cancelled = false;
    if (!colonyId) return;
    fetchEvents(colonyId, 0, 1000).then(res => {
      if (!cancelled && res?.data?.results) setHistory(res.data.results);
    });
    return () => { cancelled = true; };
  }, [colonyId]);

  const stats = useMemo(() => {
    const source = history ?? liveEvents;
    const map = new Map<string, KindStats>();
    for (const ev of source) {
      if (ev.type !== 'discovery') continue;
      const kind = String((ev.data as { critter?: string }).critter || '');
      if (!kind) continue;
      const finder = (ev as unknown as { name?: string }).name
        || (ev.lilguy ? nameFromId(ev.lilguy) : 'someone');
      const s = map.get(kind);
      if (!s) {
        map.set(kind, { count: 1, firstUnix: ev.unix, firstFinder: finder, lastUnix: ev.unix });
      } else {
        s.count++;
        if (ev.unix < s.firstUnix) { s.firstUnix = ev.unix; s.firstFinder = finder; }
        if (ev.unix > s.lastUnix) s.lastUnix = ev.unix;
      }
    }
    return map;
  }, [history, liveEvents]);

  // Reigning Bug Hunter, if the colony has crowned one
  const bugHunter = useMemo(() =>
    snapshot?.lilguys?.find(l => l.traits?.includes('catcher'))?.name ?? null,
    [snapshot]);

  const found = CRITTERS.filter(c => stats.has(c.kind)).length;

  const dateStr = (unix: number) =>
    new Date(unix * 1000).toLocaleDateString(undefined, { month: 'short', day: 'numeric' });

  return (
    <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '16px 0 4px' }}>
        <button onClick={onBack} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: SIZES.base, color: HIVE.accent, padding: 0 }}>
          &larr; Back
        </button>
      </div>
      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: palette.text, margin: '0 0 2px' }}>
        Field Guide
      </h1>
      <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginBottom: 14 }}>
        {found === 0
          ? 'No visitors spotted yet — keep watch on warm days.'
          : `${found} of ${CRITTERS.length} visitors spotted.`}
        {bugHunter && ` · Reigning Bug Hunter: ${bugHunter}`}
      </div>

      {CRITTERS.map(c => {
        const s = stats.get(c.kind);
        return (
          <Card key={c.kind} style={{ background: palette.cardBg }}>
            <div style={{ display: 'flex', gap: 12, alignItems: 'flex-start' }}>
              <div style={{
                fontSize: 34, lineHeight: '40px', minWidth: 44, textAlign: 'center',
                filter: s ? 'none' : 'grayscale(1) opacity(0.35)',
              }}>
                {c.emoji}
              </div>
              <div style={{ flex: 1 }}>
                <div style={{ fontSize: SIZES.base, fontWeight: 600, color: palette.text }}>
                  {s ? c.name : '???'}
                  {s && (
                    <span style={{ fontWeight: 400, fontSize: SIZES.sm, color: palette.dimText }}>
                      {' '}· seen {s.count} {s.count === 1 ? 'time' : 'times'}
                    </span>
                  )}
                </div>
                {s ? (
                  <>
                    <div style={{ fontSize: SIZES.sm, color: palette.text, marginTop: 2 }}>
                      {c.blurb}
                    </div>
                    <div style={{ fontSize: SIZES.xs, color: palette.dimText, marginTop: 4 }}>
                      First spotted by {s.firstFinder}, {dateStr(s.firstUnix)}
                      {' · '}last seen {dateStr(s.lastUnix)}
                    </div>
                  </>
                ) : (
                  <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginTop: 2, fontStyle: 'italic' }}>
                    {c.hint}
                  </div>
                )}
              </div>
            </div>
          </Card>
        );
      })}

      <div style={{ fontSize: SIZES.xs, color: palette.dimText, textAlign: 'center', padding: '14px 24px 0', fontStyle: 'italic' }}>
        Rarer visitors may find their way in as the colony grows&hellip;
      </div>
    </div>
  );
}
