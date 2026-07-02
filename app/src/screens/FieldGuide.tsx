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

export function FieldGuide({ onBack, initialKind }: {
  onBack: () => void;
  initialKind?: string;
}) {
  const { colonyId, events: liveEvents, snapshot } = useColony();
  const palette = TOD_PALETTES.day;
  const [history, setHistory] = useState<ColonyEvent[] | null>(null);
  const [selectedKind, setSelectedKind] = useState<string | null>(initialKind ?? null);

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

  // Critter close-up — big art, catch stats, recent finds
  // ("this is the butterfly close up... catch time, variant, number caught")
  if (selectedKind) {
    const meta = CRITTERS.find(c => c.kind === selectedKind);
    const s = stats.get(selectedKind);
    const source = history ?? liveEvents;
    const recent = source
      .filter(e => e.type === 'discovery'
        && String((e.data as { critter?: string }).critter) === selectedKind)
      .slice(-5)
      .reverse();
    const timeStr = (unix: number) =>
      new Date(unix * 1000).toLocaleString(undefined,
        { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });

    return (
      <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
        <div style={{ padding: '16px 0 4px' }}>
          <button onClick={() => setSelectedKind(null)} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: SIZES.base, color: HIVE.accent, padding: 0 }}>
            &larr; Field Guide
          </button>
        </div>

        <div style={{ textAlign: 'center', padding: '18px 0 6px' }}>
          <div style={{
            fontSize: 84, lineHeight: '96px',
            filter: s ? 'none' : 'grayscale(1) opacity(0.35)',
          }}>
            {meta?.emoji ?? '\u{1F50D}'}
          </div>
          <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: palette.text, margin: '4px 0 2px' }}>
            {s ? (meta?.name ?? selectedKind) : '???'}
          </h1>
          <div style={{ fontSize: SIZES.sm, color: palette.dimText }}>
            Common visitor {'·'} classic colouring
          </div>
        </div>

        {s ? (
          <>
            <Card style={{ background: palette.cardBg }}>
              <div style={{ display: 'flex', justifyContent: 'space-around', textAlign: 'center' }}>
                <div>
                  <div style={{ fontSize: SIZES.lg, fontWeight: 700, color: palette.text }}>{s.count}</div>
                  <div style={{ fontSize: SIZES.xs, color: palette.dimText }}>spotted</div>
                </div>
                <div>
                  <div style={{ fontSize: SIZES.lg, fontWeight: 700, color: palette.text }}>{dateStr(s.firstUnix)}</div>
                  <div style={{ fontSize: SIZES.xs, color: palette.dimText }}>first seen</div>
                </div>
                <div>
                  <div style={{ fontSize: SIZES.lg, fontWeight: 700, color: palette.text }}>{dateStr(s.lastUnix)}</div>
                  <div style={{ fontSize: SIZES.xs, color: palette.dimText }}>last seen</div>
                </div>
              </div>
              <div style={{ fontSize: SIZES.sm, color: palette.text, marginTop: 12 }}>
                {meta?.blurb}
              </div>
              <div style={{ fontSize: SIZES.xs, color: palette.dimText, marginTop: 6 }}>
                First spotted by {s.firstFinder}.
              </div>
            </Card>

            {recent.length > 0 && (
              <Card style={{ background: palette.cardBg }}>
                <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
                  Recent sightings
                </div>
                {recent.map((e, i) => (
                  <div key={i} style={{ display: 'flex', justifyContent: 'space-between', padding: '3px 0', fontSize: SIZES.sm }}>
                    <span style={{ color: palette.text }}>
                      {(e as unknown as { name?: string }).name || nameFromId(e.lilguy)}
                    </span>
                    <span style={{ color: palette.dimText, fontSize: SIZES.xs }}>{timeStr(e.unix)}</span>
                  </div>
                ))}
              </Card>
            )}
          </>
        ) : (
          <Card style={{ background: palette.cardBg }}>
            <div style={{ fontSize: SIZES.sm, color: palette.dimText, fontStyle: 'italic' }}>
              {meta?.hint ?? 'Not yet spotted.'}
            </div>
          </Card>
        )}

        <div style={{ fontSize: SIZES.xs, color: palette.dimText, textAlign: 'center', padding: '14px 24px 0', fontStyle: 'italic' }}>
          Variants with rarer colourings are rumoured&hellip;
        </div>
      </div>
    );
  }

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
          <Card key={c.kind} style={{ background: palette.cardBg }} onClick={() => setSelectedKind(c.kind)}>
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
