import { useState, useMemo, useEffect, useCallback } from 'react';
import { useColony, useColonyActions } from '../state/colony';
import { usePins } from '../state/pins';
import { useTOD } from '../state/tod';
import { Card } from '../components/Card';
import { Chip } from '../components/Chip';
import { PersonalityPetals } from '../components/PersonalityPetals';
import { BondsWeb } from '../components/BondsWeb';
import { ConkerSprite } from '../components/ConkerSprite';
import { nameFromId } from '../data/plantNames';
import { personalityPhrase, deriveRoleTag, PERSONALITY_DIMS, dimLeaning } from '../data/personality';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { fetchLilguyDetail, fetchLilguyEvents, sendCommand } from '../api/client';
import type { ColonyEvent, LilGuyDetail, Personality, Bond, ConkerMood } from '../api/types';

// Trait names arrive as firmware identifiers — translate to human warmth
// (Amber feedback: "make the traits be understandable")
const TRAIT_INFO: Record<string, { label: string; desc: string }> = {
  pioneer: { label: 'Pioneer', desc: 'First to step into an unexplored chamber.' },
  elder: { label: 'Elder', desc: 'Has lived to a grand old age — the young ones huddle close.' },
  bonded: {
    label: 'Bonded',
    desc: 'Made a true friend by spending lots of time side by side. '
        + 'Friendships can fade if they drift apart — or be lost when a friend dies — '
        + 'but the badge is theirs to keep.',
  },
  survived_heatwave: { label: 'Heatwave Survivor', desc: 'Came through a scorching spell.' },
  survived_cold_snap: { label: 'Cold Snap Survivor', desc: 'Endured a bitter freeze.' },
  survived_drought: { label: 'Drought Survivor', desc: 'Outlasted the hungry, dry days.' },
  survived_storm: { label: 'Storm Survivor', desc: 'Weathered a great storm.' },
  catcher: { label: 'Bug Hunter', desc: "The colony's reigning critter-catcher — a title that only passes to whoever out-catches the current champion." },
};

function traitLabel(t: string): string {
  return TRAIT_INFO[t]?.label ?? t.replace(/_/g, ' ');
}

// "What does this mean?" — tap to expand each personality dimension into
// the conker's leaning plus what to expect from them (Amber feedback)
function PersonalityMeanings({ personality, palette }: {
  personality: Personality;
  palette: { text: string; dimText: string };
}) {
  const [open, setOpen] = useState(false);
  return (
    <div style={{ marginTop: 10 }}>
      <button
        onClick={() => setOpen(!open)}
        style={{
          background: 'none', border: 'none', cursor: 'pointer',
          color: HIVE.accent, fontSize: SIZES.sm, padding: 0,
        }}
      >
        {open ? 'hide explanations' : 'what does this mean?'}
      </button>
      {open && PERSONALITY_DIMS.map(d => {
        const val = personality[d.key as keyof Personality];
        return (
          <div key={d.key} style={{ marginTop: 10 }}>
            <div style={{ fontSize: SIZES.sm, fontWeight: 600, color: palette.text }}>
              {d.label}: {dimLeaning(d, val)}
            </div>
            <div style={{ fontSize: SIZES.sm, color: palette.dimText }}>
              {d.meaning}
            </div>
          </div>
        );
      })}
    </div>
  );
}

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
  scale_factor?: number;
  tint_seed?: number;
  mood?: ConkerMood;
  needs?: { boredom?: number; tiredness?: number };
  deathCause?: string;
}

const MOOD_EMOJI: Record<ConkerMood, string> = {
  content: '\u{1F60C}',   // relieved
  restless: '\u{1F300}',  // cyclone (fidgety)
  bored: '\u{1F611}',     // expressionless
  playing: '\u{2728}',    // sparkles
  sleepy: '\u{1F971}',    // yawning
  happy: '\u{1F60A}',     // smiling — afterglow
  lonely: '\u{1F97A}',    // pleading — wants company
};

const MOOD_BLURB: Record<ConkerMood, string> = {
  content: 'Happy as they are right now.',
  restless: 'Getting fidgety — could use something to do.',
  bored: 'Properly bored. Give them a boop, or watch them go looking for fun.',
  playing: 'Having a great time!',
  sleepy: 'Worn out — heading for bed. A boop will rouse them if you must.',
  happy: 'Just had a lovely time — content and glowing.',
  lonely: 'Feeling a bit alone — would love some company.',
};

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
          c.bonds.push({ to: { id: targetId, name: nameFromId(targetId) }, strength: 0.5, fromHistory: true });
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

  // Snapshot roster is the source of truth for living characters.
  // Events only contribute deceased characters and history detail.
  const characters = useMemo(() => {
    const chars = new Map<number, CharacterInfo>();
    const roster = snapshot?.lilguys;

    // Living characters from snapshot roster
    const rosterIds = new Set<number>();
    if (roster) {
      for (const l of roster) {
        rosterIds.add(l.id);
        chars.set(l.id, {
          id: l.id, name: l.name, role: 'conker',
          founder: l.founder, age_days: l.age_days,
          traits: l.traits || [],
          bonds: (l.bonds || []).map(b => ({
            to: { id: b.id, name: b.name }, strength: b.strength,
            reciprocated: b.reciprocated,
          })),
          personality: l.personality,
          scale_factor: l.scale_factor,
          tint_seed: l.tint_seed,
          mood: l.mood,
          needs: l.needs,
        });
      }
    }

    // Enrich with event data (death events, bonds, personality, birth time)
    for (const ev of events) {
      if (!ev.lilguy) continue;
      const id = ev.lilguy;
      if (!chars.has(id)) {
        // Character not in roster — only add if there's a death event
        if (ev.type === 'death') {
          chars.set(id, { id, name: nameFromId(id), role: 'conker', traits: [], bonds: [] });
        } else {
          continue; // skip events for unknown characters
        }
      }
      const c = chars.get(id)!;
      switch (ev.type) {
        case 'hatch':
          c.born_unix = ev.unix;
          c.founder = !!(ev.data as { is_pioneer?: boolean }).is_pioneer;
          break;
        case 'death':
          // The living roster is authoritative: a conker present in the
          // current snapshot is alive no matter what old death events say
          // (revived conkers carry a death event in their history)
          if (!rosterIds.has(id)) {
            c.died_unix = ev.unix;
            c.deathCause = String((ev.data as { cause?: string }).cause || 'unknown');
          }
          break;
        case 'trait_earned': {
          // Roster traits are authoritative for the living — the firmware can
          // revoke a wrongly-awarded badge, but the trait_earned event stays
          // in history forever. Events only dress the deceased.
          if (rosterIds.has(id)) break;
          const trait = (ev.data as { trait?: string }).trait;
          if (trait && !c.traits.includes(String(trait))) {
            c.traits.push(String(trait));
          }
          break;
        }
        case 'bond_formed': {
          // The snapshot carries living conkers' real bonds (strength +
          // reciprocated); don't paper over them with placeholder event bonds.
          // Events only reconstruct friendships for the deceased.
          if (rosterIds.has(id)) break;
          const { target_id: targetId, target_name: targetName } =
            ev.data as { target_id?: number; target_name?: string };
          if (targetId && !c.bonds.some(b => b.to.id === targetId)) {
            // Best name wins: live roster (current, survives renames), then
            // the name recorded in the event, then the wordlist fallback
            const partner = chars.get(targetId);
            c.bonds.push({
              to: { id: targetId, name: partner?.name ?? targetName ?? nameFromId(targetId) },
              strength: 0.5,
              fromHistory: true,
            });
          }
          break;
        }
        case 'bond_broken': {
          if (rosterIds.has(id)) break;  // snapshot is authoritative for the living
          const targetId = (ev.data as { target_id?: number }).target_id;
          if (targetId) c.bonds = c.bonds.filter(b => b.to.id !== targetId);
          break;
        }
      }
    }
    return chars;
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
        if (r) setLilguyEvents(r.results.filter(e => e.type !== 'chamber_crossing'));
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

      {/* Her Majesty — first among equals (feedback: queen belongs in the list) */}
      {filter !== 'deceased' && snapshot?.queen_name && (
        <Card style={{
          background: palette.cardBg,
          display: 'flex', alignItems: 'center', gap: 12,
          borderLeft: `3px solid ${HIVE.accent}`,
        }}>
          <img
            src="/app/queen.png"
            alt="Queen"
            style={{ width: 44, height: 44, imageRendering: 'pixelated', flexShrink: 0 }}
          />
          <div>
            <div style={{ fontSize: SIZES.base, fontWeight: 600, color: palette.text }}>
              Queen {snapshot.queen_name}
            </div>
            <div style={{ fontSize: SIZES.sm, color: palette.dimText }}>
              Mother of the colony
              {snapshot.founded_unix
                ? ` · reigning ${Math.max(1, Math.round((Date.now() / 1000 - snapshot.founded_unix) / 86400))} day${Math.round((Date.now() / 1000 - snapshot.founded_unix) / 86400) !== 1 ? 's' : ''}`
                : ''}
            </div>
          </div>
        </Card>
      )}

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
              {c.traits.length > 0 && ` \u00b7 ${c.traits.map(traitLabel).join(', ')}`}
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
  const { colonyId } = useColony();
  const [renaming, setRenaming] = useState(false);

  const handleRename = useCallback(async () => {
    if (!colonyId) return;
    const input = window.prompt(`New name for ${char.name} (letters, max 15):`, char.name);
    if (!input) return;
    const trimmed = input.trim().slice(0, 15);
    if (!trimmed || trimmed === char.name) return;
    setRenaming(true);
    const ok = await sendCommand(colonyId, 'name_conker', { id: char.id, name: trimmed });
    setRenaming(false);
    window.alert(ok
      ? `Sent! ${char.name} will become "${trimmed}" within a minute.`
      : 'Could not reach the colony server — try again later.');
  }, [colonyId, char.id, char.name]);

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
        {!isDeceased && (
          // A proper labelled pill — the bare pencil was invisible (Amber)
          <button
            onClick={handleRename}
            disabled={renaming}
            aria-label="Rename"
            style={{
              background: 'none',
              border: `1.5px solid ${HIVE.accent}`,
              borderRadius: 12,
              cursor: 'pointer',
              fontSize: SIZES.sm,
              fontWeight: 600,
              color: HIVE.accent,
              padding: '3px 10px',
              marginLeft: 10,
              opacity: renaming ? 0.4 : 1,
              verticalAlign: 'middle',
            }}
          >
            {'✎'} Rename
          </button>
        )}
      </h1>
      <div style={{ fontSize: SIZES.base, color: palette.dimText, marginBottom: 16 }}>
        {deriveRoleTag(personality, char.role, char.traits)}
        {personality && ` \u00b7 ${personalityPhrase(personality)}`}
        {isDeceased && (
          <span style={{ color: HIVE.dimText }}> \u00b7 Deceased</span>
        )}
      </div>

      {/* Conker sprite with scale */}
      {(char.scale_factor || char.tint_seed) && (
        <Card style={{ background: palette.cardBg }}>
          <ConkerSprite
            scaleFactor={char.scale_factor}
            tintSeed={char.tint_seed}
            palette={palette}
          />
        </Card>
      )}

      {/* Mood & needs (live conkers only) */}
      {!isDeceased && char.mood && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, color: palette.dimText, marginBottom: 8 }}>
            Mood &amp; needs
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 4 }}>
            <span style={{ fontSize: 22 }}>{MOOD_EMOJI[char.mood]}</span>
            <div>
              <div style={{ fontSize: SIZES.base, color: palette.text, textTransform: 'capitalize' }}>
                {char.mood}
              </div>
              <div style={{ fontSize: SIZES.xs, color: palette.dimText }}>
                {MOOD_BLURB[char.mood]}
              </div>
            </div>
          </div>
          {typeof char.needs?.boredom === 'number' && (
            <div style={{ marginTop: 10 }}>
              <div style={{ display: 'flex', justifyContent: 'space-between',
                            fontSize: SIZES.xs, color: palette.dimText, marginBottom: 3 }}>
                <span>Restlessness</span>
                <span>{Math.round(char.needs.boredom * 100)}%</span>
              </div>
              <div style={{ height: 6, borderRadius: 3, background: HIVE.parchment, overflow: 'hidden' }}>
                <div style={{
                  width: `${Math.round(Math.min(1, char.needs.boredom) * 100)}%`,
                  height: '100%',
                  background: char.needs.boredom >= 0.85 ? HIVE.alert
                            : char.needs.boredom >= 0.55 ? HIVE.accent
                            : HIVE.leafGreen,
                }} />
              </div>
            </div>
          )}
          {typeof char.needs?.tiredness === 'number' && (
            <div style={{ marginTop: 10 }}>
              <div style={{ display: 'flex', justifyContent: 'space-between',
                            fontSize: SIZES.xs, color: palette.dimText, marginBottom: 3 }}>
                <span>Tiredness</span>
                <span>{Math.round(char.needs.tiredness * 100)}%</span>
              </div>
              <div style={{ height: 6, borderRadius: 3, background: HIVE.parchment, overflow: 'hidden' }}>
                <div style={{
                  width: `${Math.round(Math.min(1, char.needs.tiredness) * 100)}%`,
                  height: '100%',
                  background: char.needs.tiredness >= 0.7 ? HIVE.sky : HIVE.bark,
                }} />
              </div>
            </div>
          )}
        </Card>
      )}

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
        </div>
      </Card>

      {/* Traits — friendly names with what-they-mean */}
      {char.traits.length > 0 && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
            Traits
          </div>
          {char.traits.map(t => {
            const info = TRAIT_INFO[t];
            // Bonded names its friends right here (feedback: "says bonded but
            // doesn't tell you who they're bonded to")
            let desc = info?.desc;
            if (t === 'bonded' && bonds.length > 0) {
              const names = bonds.map(b => b.to.name);
              desc = names.length === 1
                ? `Best friends with ${names[0]}.`
                : `Best friends with ${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}.`;
            }
            return (
              <div key={t} style={{ marginBottom: 8 }}>
                <div style={{ fontSize: SIZES.sm, fontWeight: 600, color: palette.text }}>
                  {'\u{1F3C5}'} {info?.label ?? t.replace(/_/g, ' ')}
                </div>
                {desc && (
                  <div style={{ fontSize: SIZES.sm, color: palette.dimText }}>
                    {desc}
                  </div>
                )}
              </div>
            );
          })}
        </Card>
      )}

      {/* Personality petals + what-it-means */}
      {personality && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
            Personality
          </div>
          <div style={{ display: 'flex', justifyContent: 'center' }}>
            <PersonalityPetals personality={personality} size={180} />
          </div>
          <PersonalityMeanings personality={personality} palette={palette} />
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
          <div style={{ marginTop: 8 }}>
            {[...bonds].sort((a, b) => Number(b.reciprocated) - Number(a.reciprocated) || b.strength - a.strength).map((b, i) => (
              <div key={i} style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', padding: '4px 0' }}>
                <span style={{ fontSize: SIZES.sm, color: palette.text, fontWeight: 500 }}>
                  {b.reciprocated && <span title="best friends" style={{ marginRight: 4 }}>⭐</span>}
                  {b.to.name}
                </span>
                <span style={{ fontSize: SIZES.xs, color: palette.dimText }}>
                  {b.reciprocated ? 'best friends' : 'has a soft spot for'}
                  {!b.fromHistory && ` · ${Math.round(b.strength * 100)}%`}
                </span>
              </div>
            ))}
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
