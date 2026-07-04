import { useEffect, useMemo, useState, type ReactNode } from 'react';
import { useColony } from '../state/colony';
import { Card } from '../components/Card';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { fetchEvents } from '../api/client';
import { composeSaga, type Chapter } from '../data/saga';
import {
  loadAnnals, computeObservances, annalsDate,
  ARC_ORDER, ARC_LABELS, type Deed, type Era, type Annals, type Observance,
} from '../data/chronicle';
import type { ColonyEvent } from '../api/types';

export type ChronicleSection = 'saga' | 'deeds' | 'renown';

const WEEK = 7 * 86400;

// The Chronicle — the colony's history as its progression system. Three
// leaves of one book: the Saga (weekly chapters under era headers), the
// Deeds ledger (recorded pages and rumoured ones), and Renown (the
// reputation the colony itself accrues, plus its calendar of observances).
export function Saga({ onBack, initialSection, initialDeedId }: {
  onBack: () => void;
  initialSection?: ChronicleSection;
  initialDeedId?: string;
}) {
  const { colonyId, events: liveEvents, snapshot } = useColony();
  const palette = TOD_PALETTES.day;
  const [history, setHistory] = useState<ColonyEvent[] | null>(null);
  const [section, setSection] = useState<ChronicleSection>(initialSection ?? 'saga');

  useEffect(() => {
    let cancelled = false;
    if (!colonyId) return;
    fetchEvents(colonyId, 0, 1000).then(res => {
      if (!cancelled && res?.data?.results) setHistory(res.data.results);
    });
    return () => { cancelled = true; };
  }, [colonyId]);

  const events = history ?? liveEvents;

  const annals = useMemo(
    () => (colonyId ? loadAnnals(colonyId, events, snapshot) : null),
    [colonyId, events, snapshot]);

  const chapters = useMemo(
    () => composeSaga(events, snapshot?.founded_unix, annals?.deeds, annals?.eras),
    [events, snapshot, annals]);

  const observances = useMemo(
    () => computeObservances(events, snapshot, annals),
    [events, snapshot, annals]);

  // Deep link: land on a deed row (from a Home card or the deed toast)
  useEffect(() => {
    if (!initialDeedId) return;
    const t = setTimeout(() => {
      document.getElementById(`deed-${initialDeedId}`)
        ?.scrollIntoView({ behavior: 'smooth', block: 'center' });
    }, 80);
    return () => clearTimeout(t);
  }, [initialDeedId, section]);

  const jumpToChapter = (startUnix: number) => {
    setSection('saga');
    setTimeout(() => {
      document.getElementById(`chapter-${startUnix}`)
        ?.scrollIntoView({ behavior: 'smooth', block: 'start' });
    }, 80);
  };

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
        The Chronicle of {colonyId}
      </h1>
      <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginBottom: 10, fontStyle: 'italic' }}>
        As the chronicle tells it.
      </div>

      {/* Segmented control */}
      <div style={{
        display: 'flex', gap: 4, marginBottom: 14, padding: 3,
        background: palette.cardBg, borderRadius: 12,
      }}>
        {(['saga', 'deeds', 'renown'] as ChronicleSection[]).map(s => (
          <button
            key={s}
            onClick={() => setSection(s)}
            style={{
              flex: 1, padding: '7px 0', border: 'none', borderRadius: 9,
              cursor: 'pointer', fontSize: SIZES.sm, fontWeight: 600,
              background: section === s ? HIVE.accent : 'transparent',
              color: section === s ? HIVE.white : palette.dimText,
            }}
          >
            {s === 'saga' ? 'Saga' : s === 'deeds' ? 'Deeds' : 'Renown'}
          </button>
        ))}
      </div>

      {section === 'saga' && (
        <SagaSection chapters={chapters} eras={annals?.eras ?? []} palette={palette} dateStr={dateStr} />
      )}
      {section === 'deeds' && annals && (
        <DeedsSection
          annals={annals}
          events={events}
          chapters={chapters}
          onChapterJump={jumpToChapter}
          palette={palette}
        />
      )}
      {section === 'renown' && annals && (
        <RenownSection
          colonyId={colonyId || 'The colony'}
          annals={annals}
          observances={observances}
          palette={palette}
        />
      )}
    </div>
  );
}

type Palette = { bg: string; cardBg: string; text: string; dimText: string };

// ---- Saga: weekly chapters grouped under era headers ----

function SagaSection({ chapters, eras, palette, dateStr }: {
  chapters: Chapter[];
  eras: Era[];
  palette: Palette;
  dateStr: (unix: number) => string;
}) {
  const eraFor = (unix: number): Era | null => {
    let best: Era | null = null;
    for (const e of eras) {
      if (e.startUnix <= unix && (!best || e.startUnix > best.startUnix)) best = e;
    }
    return best ?? (eras.length > 0 ? eras[0] : null);
  };

  if (chapters.length === 0) {
    return (
      <div style={{ color: palette.dimText, fontSize: SIZES.base, textAlign: 'center', padding: 32 }}>
        The story hasn’t started yet.
      </div>
    );
  }

  let lastEraName: string | null = null;
  const rows: ReactNode[] = [];
  for (const ch of chapters) {
    const era = eraFor(ch.startUnix);
    if (era && era.name !== lastEraName) {
      lastEraName = era.name;
      rows.push(
        <div key={`era-${era.startUnix}`} style={{ margin: '18px 2px 8px' }}>
          <div style={{
            fontSize: SIZES.sm, fontWeight: 700, color: palette.text,
            textTransform: 'uppercase', letterSpacing: 1.5,
          }}>
            {era.name}
          </div>
          <div style={{ fontSize: SIZES.xs, color: palette.dimText, marginTop: 2 }}>
            {dateStr(era.startUnix)} — {era.endUnix ? dateStr(era.endUnix) : 'present'}
          </div>
          {era.summary && (
            <div style={{ fontSize: SIZES.xs, color: palette.dimText, marginTop: 2, fontStyle: 'italic' }}>
              {era.summary}
            </div>
          )}
        </div>,
      );
    }
    rows.push(
      <Card key={ch.startUnix} style={{ background: palette.cardBg, padding: 16 }}>
        <div id={`chapter-${ch.startUnix}`} style={{ fontSize: SIZES.base, fontWeight: 700, color: palette.text }}>
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
      </Card>,
    );
  }

  return (
    <>
      {rows}
      <div style={{ fontSize: SIZES.xs, color: palette.dimText, textAlign: 'center', padding: '14px 24px 0', fontStyle: 'italic' }}>
        New chapters write themselves as the weeks turn.
      </div>
    </>
  );
}

// ---- Deeds: the ledger, grouped by arc ----

function DeedsSection({ annals, events, chapters, onChapterJump, palette }: {
  annals: Annals;
  events: ColonyEvent[];
  chapters: Chapter[];
  onChapterJump: (chapterStartUnix: number) => void;
  palette: Palette;
}) {
  const chapterFor = (unix: number): Chapter | undefined =>
    chapters.find(ch => unix >= ch.startUnix && unix < ch.startUnix + WEEK);

  // Marginalia: firmware milestone events, verbatim-but-warmed. Any kind
  // renders generically, so future firmware kinds appear for free.
  const marginalia = useMemo(() => {
    return events
      .filter(e => e.type === 'milestone')
      .sort((a, b) => b.unix - a.unix)
      .slice(0, 6);
  }, [events]);

  return (
    <>
      {ARC_ORDER.map(arc => {
        const visible = annals.deeds.filter(dd => dd.arc === arc && dd.state !== 'unwritten');
        if (visible.length === 0) return null;
        return (
          <div key={arc}>
            <div style={{
              fontSize: SIZES.xs, fontWeight: 700, color: palette.dimText,
              textTransform: 'uppercase', letterSpacing: 1.5, margin: '16px 2px 8px',
            }}>
              {ARC_LABELS[arc]}
            </div>
            <Card style={{ background: palette.cardBg, padding: '6px 16px' }}>
              {visible.map(dd => (
                <DeedRow
                  key={dd.id}
                  deed={dd}
                  chapter={dd.unix ? chapterFor(dd.unix) : undefined}
                  onChapterJump={onChapterJump}
                  palette={palette}
                />
              ))}
            </Card>
          </div>
        );
      })}

      {marginalia.length > 0 && (
        <>
          <div style={{
            fontSize: SIZES.xs, fontWeight: 700, color: palette.dimText,
            textTransform: 'uppercase', letterSpacing: 1.5, margin: '16px 2px 8px',
          }}>
            Marginalia
          </div>
          <Card style={{ background: palette.cardBg }}>
            {marginalia.map((ev, i) => {
              const data = ev.data as Record<string, unknown>;
              const kind = String(data.kind || 'a moment').replace(/_/g, ' ');
              const value = data.value !== undefined ? ` (${String(data.value)})` : '';
              return (
                <div key={i} style={{ fontSize: SIZES.sm, color: palette.text, padding: '3px 0' }}>
                  The chronicle notes: {kind}{value} — {annalsDate(ev.unix)}.
                </div>
              );
            })}
          </Card>
        </>
      )}

      {annals.truncated && annals.oldestEventUnix && (
        <div style={{ fontSize: SIZES.xs, color: palette.dimText, textAlign: 'center', padding: '14px 24px 0', fontStyle: 'italic' }}>
          The chronicle’s records begin {annalsDate(annals.oldestEventUnix)}. Earlier days
          live on in the colony’s own memory.
        </div>
      )}
    </>
  );
}

function DeedRow({ deed, chapter, onChapterJump, palette }: {
  deed: Deed;
  chapter?: Chapter;
  onChapterJump: (chapterStartUnix: number) => void;
  palette: Palette;
}) {
  const recorded = deed.state === 'recorded';
  return (
    <div id={`deed-${deed.id}`} style={{ display: 'flex', gap: 10, padding: '9px 0', alignItems: 'flex-start' }}>
      <div style={{
        fontSize: 18, lineHeight: '22px', minWidth: 24, textAlign: 'center',
        filter: recorded ? 'none' : 'grayscale(1) opacity(0.4)',
      }}>
        {recorded ? deed.icon : '\u{2712}\u{FE0E}'}
      </div>
      <div style={{ flex: 1 }}>
        <div style={{
          fontSize: SIZES.sm, fontWeight: 700, letterSpacing: 0.6,
          fontVariant: 'small-caps',
          color: recorded ? palette.text : palette.dimText,
        }}>
          {deed.title.toLowerCase()}
        </div>
        {recorded ? (
          <div style={{ fontSize: SIZES.sm, color: palette.text, lineHeight: 1.45, marginTop: 1 }}>
            {deed.inscription}, {deed.unix ? annalsDate(deed.unix) : ''}.
            {chapter && (
              <button
                onClick={() => onChapterJump(chapter.startUnix)}
                style={{
                  background: 'none', border: 'none', cursor: 'pointer', padding: '0 0 0 6px',
                  fontSize: SIZES.xs, color: HIVE.accent, fontWeight: 600,
                }}
              >
                &rarr; {chapter.title.split(' — ')[0]}
              </button>
            )}
          </div>
        ) : (
          <div style={{ fontSize: SIZES.sm, color: palette.dimText, fontStyle: 'italic', lineHeight: 1.45, marginTop: 1 }}>
            {deed.hint}
          </div>
        )}
      </div>
    </div>
  );
}

// ---- Renown: standing, families, observances ----

function RenownSection({ colonyId, annals, observances, palette }: {
  colonyId: string;
  annals: Annals;
  observances: Observance[];
  palette: Palette;
}) {
  const upcoming = observances.slice(0, 3);
  const shortDate = (unix: number) =>
    new Date(unix * 1000).toLocaleDateString(undefined, { day: 'numeric', month: 'short' });

  return (
    <>
      {/* Banner */}
      <Card style={{ background: palette.cardBg, textAlign: 'center', padding: 20 }}>
        {annals.title ? (
          <>
            <div style={{ fontSize: SIZES.md, fontWeight: 700, color: palette.text }}>
              {colonyId}
            </div>
            <div style={{ fontSize: SIZES.base, color: palette.text, fontStyle: 'italic', marginTop: 2 }}>
              {annals.title}
            </div>
          </>
        ) : (
          <div style={{ fontSize: SIZES.sm, color: palette.dimText, fontStyle: 'italic', lineHeight: 1.5 }}>
            The colony is young. Renown is earned, not given — the first pages
            are blank on purpose.
          </div>
        )}
      </Card>

      {annals.families.map(fam => (
        <Card key={fam.id} style={{ background: palette.cardBg }}>
          <div style={{
            fontSize: SIZES.xs, fontWeight: 700, color: palette.dimText,
            textTransform: 'uppercase', letterSpacing: 1.5, marginBottom: 4,
          }}>
            {fam.label}
          </div>
          {fam.standing && (
            <div style={{ fontSize: SIZES.base, color: palette.text, fontStyle: 'italic' }}>
              “{fam.standing}”
            </div>
          )}
          <div style={{ fontSize: SIZES.sm, color: palette.dimText, marginTop: 4, lineHeight: 1.45 }}>
            {fam.nextHint}
          </div>
          {fam.earned.length > 0 && (
            <div style={{ marginTop: 8 }}>
              {fam.earned.map(t => (
                <div key={t.tier} style={{ fontSize: SIZES.xs, color: palette.dimText, padding: '1px 0' }}>
                  “{t.phrase}” — {annalsDate(t.unix)}
                </div>
              ))}
            </div>
          )}
        </Card>
      ))}

      {/* Observances calendar */}
      <div style={{
        fontSize: SIZES.xs, fontWeight: 700, color: palette.dimText,
        textTransform: 'uppercase', letterSpacing: 1.5, margin: '16px 2px 8px',
      }}>
        Observances
      </div>
      <Card style={{ background: palette.cardBg }}>
        {upcoming.length === 0 ? (
          <div style={{ fontSize: SIZES.sm, color: palette.dimText, fontStyle: 'italic' }}>
            The calendar is quiet for now; the first anniversaries are still on their way.
          </div>
        ) : (
          upcoming.map((o, i) => (
            <div key={i} style={{ display: 'flex', gap: 10, padding: '4px 0', alignItems: 'baseline' }}>
              <div style={{ fontSize: SIZES.xs, color: HIVE.accent, fontWeight: 600, minWidth: 52 }}>
                {shortDate(o.unix)}
              </div>
              <div style={{ fontSize: SIZES.sm, color: palette.text, lineHeight: 1.4 }}>
                {o.line}
              </div>
            </div>
          ))
        )}
      </Card>
    </>
  );
}
