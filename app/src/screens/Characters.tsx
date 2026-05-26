import { useState, useMemo, useEffect, useCallback } from 'react';
import { useColony, useColonyActions } from '../state/colony';
import { usePins } from '../state/pins';
import { useTOD } from '../state/tod';
import { Card } from '../components/Card';
import { Chip } from '../components/Chip';
import { PersonalityPetals } from '../components/PersonalityPetals';
import { BondsWeb } from '../components/BondsWeb';
import { nameFromId } from '../data/plantNames';
import { personalityPhrase, deriveRoleTag } from '../data/personality';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { fetchLilguyDetail, fetchLilguyEvents } from '../api/client';
import type { ColonyEvent, LilGuyDetail, Personality, Bond } from '../api/types';

// Reconstruct basic character info from events
interface CharacterInfo {
  id: number;
  name: string;
  role: 'conker';
  founder?: boolean;
  born_unix?: number;
  died_unix?: number;
  age_days?: number;
  traits: string[];
  bonds: Bond[];
  personality?: Personality;
  deathCause?: string;
}

function buildCharactersFromEvents(events: ColonyEvent[]): Map<number, CharacterInfo> {
  const chars = new Map<number, CharacterInfo>();

  for (const ev of events) {
    if (!ev.lilguy) continue;
    const id = ev.lilguy;
    if (!chars.has(id)) {
      chars.set(id, { id, name: nameFromId(id), role: 'conker', traits: [], bonds: [] });
    }
    const c = chars.get(id)!;

    switch (ev.type) {
      case 'hatch':
        c.born_unix = ev.unix;
        c.role = 'conker';
        c.founder = !!(ev.data as { is_pioneer?: boolean }).is_pioneer;
        break;
      case 'death':
        c.died_unix = ev.unix;
        c.deathCause = String((ev.data as { cause?: string }).cause || 'unknown');
        break;
      case 'role_change':
        c.role = 'conker';
        break;
      case 'trait_earned':
        if ((ev.data as { trait?: string }).trait) {
          c.traits.push(String((ev.data as { trait: string }).trait));
        }
        break;
      case 'bond_formed': {
        const targetId = (ev.data as { target_id?: number }).target_id;
        if (targetId && !c.bonds.some(b => b.to.id === targetId)) {
          c.bonds.push({ to: { id: targetId, name: nameFromId(targetId) }, strength: 0.5 });
        }
        break;
      }
    }
  }
  return chars;
}

interface CharactersProps {
  onNavigate: (tab: string, params?: Record<string, unknown>) => void;
  initialLilguyId?: number;
}

export function Characters({ onNavigate, initialLilguyId }: CharactersProps) {
  const { events, colonyId } = useColony();
  const { isPinned, togglePin } = usePins();
  const palette = TOD_PALETTES.day; // Reflective screen — always day palette

  const [selectedId, setSelectedId] = useState<number | null>(initialLilguyId ?? null);
  const [filter, setFilter] = useState<'alive' | 'deceased' | 'all'>('alive');
  const [detail, setDetail] = useState<LilGuyDetail | null>(null);
  const [lilguyEvents, setLilguyEvents] = useState<ColonyEvent[]>([]);

  const { snapshot } = useColony();

  // Use snapshot roster as source of truth (available via VPS and LAN)
  const characters = useMemo(() => {
    const fromEvents = buildCharactersFromEvents(events);
    const roster = snapshot?.lilguys;
    if (roster && roster.length > 0) {
      const liveIds = new Set(roster.map(l => l.id));
      // Mark characters not in live roster as deceased
      for (const [id, c] of fromEvents) {
        if (!liveIds.has(id) && !c.died_unix) {
          c.died_unix = -1; // gone but no death event
        }
      }
      // Add roster entries missing from events
      for (const l of roster) {
        if (!fromEvents.has(l.id)) {
          fromEvents.set(l.id, {
            id: l.id, name: l.name, role: 'conker',
            founder: l.founder, age_days: l.age_days,
            traits: l.traits, bonds: [],
          });
        } else {
          // Update age from roster (more accurate than events)
          const c = fromEvents.get(l.id)!;
          c.age_days = l.age_days;
          c.founder = l.founder;
        }
      }
    }
    return fromEvents;
  }, [events, snapshot]);

  const charList = useMemo(() => {
    const list = Array.from(characters.values());
    if (filter === 'alive') return list.filter(c => !c.died_unix);
    if (filter === 'deceased') return list.filter(c => c.died_unix);
    return list;
  }, [characters, filter]);

  // Load detail from LAN when a character is selected
  useEffect(() => {
    if (selectedId === null) {
      setDetail(null);
      setLilguyEvents([]);
      return;
    }
    fetchLilguyDetail(selectedId).then(d => d && setDetail(d));
    if (colonyId) {
      fetchLilguyEvents(colonyId, selectedId).then(r => {
        if (r) setLilguyEvents(r.results);
      });
    }
  }, [selectedId, colonyId]);

  // Handle initialLilguyId changes
  useEffect(() => {
    if (initialLilguyId !== undefined) setSelectedId(initialLilguyId);
  }, [initialLilguyId]);

  if (selectedId !== null) {
    const char = characters.get(selectedId);
    return (
      <CharacterProfile
        char={char || { id: selectedId, name: nameFromId(selectedId), role: 'conker', traits: [], bonds: [] }}
        detail={detail}
        events={lilguyEvents}
        isPinned={isPinned(selectedId)}
        onTogglePin={() => togglePin(selectedId)}
        onBack={() => setSelectedId(null)}
        palette={palette}
      />
    );
  }

  return (
    <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: palette.text, padding: '16px 0 8px' }}>
        Characters
      </h1>

      {/* Filters */}
      <div style={{ display: 'flex', gap: 8, marginBottom: 12 }}>
        {(['alive', 'deceased', 'all'] as const).map(f => (
          <Chip key={f} label={f.charAt(0).toUpperCase() + f.slice(1)} active={filter === f} onClick={() => setFilter(f)} />
        ))}
      </div>

      {charList.length === 0 && (
        <div style={{ color: palette.dimText, fontSize: SIZES.base, textAlign: 'center', padding: 32 }}>
          {filter === 'alive' ? 'No living characters found' : 'No characters found'}
        </div>
      )}

      {charList.map(c => (
        <Card
          key={c.id}
          style={{
            background: palette.cardBg,
            display: 'flex', alignItems: 'center', justifyContent: 'space-between',
            opacity: c.died_unix ? 0.7 : 1,
          }}
          onClick={() => setSelectedId(c.id)}
        >
          <div>
            <div style={{ fontSize: SIZES.base, fontWeight: 600, color: palette.text }}>
              {c.name}
              {c.died_unix && <span style={{ fontSize: SIZES.xs, color: palette.dimText, marginLeft: 6 }}>(deceased)</span>}
            </div>
            <div style={{ fontSize: SIZES.sm, color: palette.dimText }}>
              {deriveRoleTag(c.personality, c.role, c.traits)}
              {c.traits.length > 0 && ` \u00b7 ${c.traits.join(', ')}`}
            </div>
          </div>
          <button
            onClick={(e) => { e.stopPropagation(); togglePin(c.id); }}
            style={{
              background: 'none', border: 'none', cursor: 'pointer',
              fontSize: 18, color: isPinned(c.id) ? HIVE.accent : HIVE.sand,
              padding: 4,
            }}
            aria-label={isPinned(c.id) ? 'Unpin' : 'Pin'}
          >
            {isPinned(c.id) ? '\u2605' : '\u2606'}
          </button>
        </Card>
      ))}
    </div>
  );
}

// ---- Character Profile ----

function CharacterProfile({ char, detail, events, isPinned, onTogglePin, onBack, palette }: {
  char: CharacterInfo;
  detail: LilGuyDetail | null;
  events: ColonyEvent[];
  isPinned: boolean;
  onTogglePin: () => void;
  onBack: () => void;
  palette: { bg: string; cardBg: string; text: string; dimText: string };
}) {
  const personality = detail?.personality || char.personality;
  const bonds = detail?.bonds || char.bonds;
  const isDeceased = !!char.died_unix || !!detail?.died_unix;

  const ageText = useMemo(() => {
    // age_days from firmware is sim-running time only (pauses when module is off)
    const days = detail?.age_days ?? char.age_days;
    if (days === undefined) return null;
    return `${days.toFixed(1)} days`;
  }, [detail, char]);

  return (
    <div style={{ background: palette.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      {/* Back + Pin */}
      <div style={{ display: 'flex', justifyContent: 'space-between', padding: '16px 0 8px' }}>
        <button onClick={onBack} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: SIZES.base, color: HIVE.accent }}>
          &larr; Back
        </button>
        <button onClick={onTogglePin} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: 20, color: isPinned ? HIVE.accent : HIVE.sand }}>
          {isPinned ? '\u2605' : '\u2606'}
        </button>
      </div>

      {/* Name + role */}
      <h1 style={{ fontSize: SIZES.xxl, fontWeight: 700, color: palette.text, margin: 0 }}>
        {char.name}
      </h1>
      <div style={{ fontSize: SIZES.base, color: palette.dimText, marginBottom: 16 }}>
        {deriveRoleTag(personality, char.role, char.traits)}
        {personality && ` \u00b7 ${personalityPhrase(personality)}`}
        {isDeceased && (
          <span style={{ color: HIVE.dimText }}> \u00b7 Deceased</span>
        )}
      </div>

      {/* Memorial banner for deceased */}
      {isDeceased && (
        <Card style={{ background: '#E8DCC8', borderLeft: `3px solid ${HIVE.bark}` }}>
          <div style={{ fontSize: SIZES.sm, color: HIVE.soil, fontStyle: 'italic' }}>
            {char.deathCause === 'starvation'
              ? `Perished from starvation after ${ageText || 'an unknown time'}.`
              : `Lived ${ageText || 'a full life'}. Died of natural causes.`}
          </div>
        </Card>
      )}

      {/* Stats */}
      <Card style={{ background: palette.cardBg }}>
        <div style={{ display: 'flex', gap: 24 }}>
          {ageText && <MiniStat label="Age" value={ageText} palette={palette} />}
          <MiniStat label="Role" value={char.role} palette={palette} />
          {char.traits.length > 0 && (
            <MiniStat label="Traits" value={char.traits.join(', ')} palette={palette} />
          )}
        </div>
      </Card>

      {/* Personality petals */}
      {personality && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
            Personality
          </div>
          <div style={{ display: 'flex', justifyContent: 'center' }}>
            <PersonalityPetals personality={personality} size={180} />
          </div>
        </Card>
      )}

      {!personality && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.sm, color: palette.dimText, textAlign: 'center', padding: 12 }}>
            Personality data available when connected to the queen (LAN)
          </div>
        </Card>
      )}

      {/* Bonds */}
      {bonds.length > 0 && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
            Bonds
          </div>
          <div style={{ display: 'flex', justifyContent: 'center' }}>
            <BondsWeb name={char.name} bonds={bonds} />
          </div>
        </Card>
      )}

      {/* Lineage */}
      {detail?.tended_by && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 4 }}>
            Lineage
          </div>
          <div style={{ fontSize: SIZES.base, color: palette.text }}>
            Tended by <span style={{ fontWeight: 600 }}>{detail.tended_by.name}</span>
          </div>
        </Card>
      )}

      {/* Events */}
      {events.length > 0 && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
            Life Events
          </div>
          {events.slice(-20).reverse().map((ev, i) => (
            <EventRow key={i} event={ev} palette={palette} />
          ))}
        </Card>
      )}
    </div>
  );
}

function MiniStat({ label, value, palette }: {
  label: string; value: string; palette: { text: string; dimText: string };
}) {
  return (
    <div>
      <div style={{ fontSize: SIZES.xs, color: palette.dimText }}>{label}</div>
      <div style={{ fontSize: SIZES.sm, fontWeight: 600, color: palette.text }}>{value}</div>
    </div>
  );
}

function EventRow({ event, palette }: {
  event: ColonyEvent;
  palette: { text: string; dimText: string };
}) {
  const date = new Date(event.unix * 1000);
  const dateStr = `${date.getMonth() + 1}/${date.getDate()}`;
  return (
    <div style={{ display: 'flex', alignItems: 'baseline', gap: 8, padding: '3px 0', fontSize: SIZES.sm }}>
      <span style={{ color: palette.dimText, minWidth: 36 }}>{dateStr}</span>
      <span style={{ color: palette.text }}>{formatEventType(event.type)}</span>
    </div>
  );
}

function formatEventType(type: string): string {
  return type.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase());
}
