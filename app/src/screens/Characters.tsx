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
import { TRAIT_INFO, traitLabel } from '../data/traits';
import { epithet, buildLifeStory } from '../data/biography';
import { HIVE, TOD_PALETTES } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { fetchLilguyDetail, fetchLilguyEvents, sendCommand } from '../api/client';
import type { ColonyEvent, LilGuyDetail, Personality, Bond, ConkerMood, ConkerActivity } from '../api/types';

// Trait metadata now lives in data/traits.ts (shared with biography.ts).

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
  needs?: { boredom?: number; tiredness?: number; loneliness?: number };
  activity?: ConkerActivity;
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

// What they're up to right now (firmware ConkerActivity). Emoji + short title +
// a flavourful, name-prefixed blurb.
const ACTIVITY_EMOJI: Record<ConkerActivity, string> = {
  idling: '\u{1F33C}',           // blossom
  sleeping: '\u{1F634}',         // sleeping face
  napping: '\u{1F4A4}',          // zzz
  seeking_company: '\u{1F97A}',  // pleading
  foraging: '\u{1F50E}',         // magnifier
  carrying_food: '\u{1F330}',    // chestnut (a conker!)
  heading_home: '\u{1F3E0}',     // house
  eating: '\u{1F37D}\u{FE0F}',   // plate & cutlery
  chasing_firefly: '\u{2728}',   // sparkles
  playing: '\u{1F4A8}',          // dash
  parading: '\u{1F389}',         // party popper
  sowing: '\u{1F331}',           // seedling
  gardening: '\u{1FAB4}',        // potted plant
  heading_to_garden: '\u{1F6B6}',// walker
  tending_brood: '\u{1F95A}',    // egg
  feeding_queen: '\u{1F451}',    // crown
  crafting: '\u{1F3A8}',         // palette
  mourning: '\u{1F56F}\u{FE0F}', // candle
  clearing: '\u{1F9F9}',         // broom
  away: '\u{1F32B}\u{FE0F}',     // fog
};

const ACTIVITY_TITLE: Record<ConkerActivity, string> = {
  idling: 'Pottering about',
  sleeping: 'Fast asleep',
  napping: 'Having a nap',
  seeking_company: 'Looking for company',
  foraging: 'Foraging',
  carrying_food: 'Carrying food home',
  heading_home: 'Heading home',
  eating: 'Having a bite',
  chasing_firefly: 'Chasing a firefly',
  playing: 'Playing chase',
  parading: 'Leading a parade',
  sowing: 'Sowing a seed',
  gardening: 'Minding the garden',
  heading_to_garden: 'Off to the garden',
  tending_brood: 'Tending the brood',
  feeding_queen: 'Feeding the queen',
  crafting: 'Crafting',
  mourning: 'In mourning',
  clearing: 'Tidying up',
  away: 'Away',
};

// Prefixed with the conker's name → "Marigold is out on the trail, foraging…"
const ACTIVITY_BLURB: Record<ConkerActivity, string> = {
  idling: 'is pottering about with nothing much to do.',
  sleeping: 'is fast asleep, dreaming of crumbs.',
  napping: 'is curled up for a little daytime nap.',
  seeking_company: 'is feeling a bit alone, off to find a friend to huddle with.',
  foraging: 'is out on the trail, foraging for food.',
  carrying_food: 'is hauling a hard-won morsel back to the nest.',
  heading_home: 'is ambling back home.',
  eating: 'is tucking into a well-earned meal.',
  chasing_firefly: 'is darting through the dark, chasing a firefly.',
  playing: 'is full of beans, zooming about after a friend.',
  parading: 'is leading a merry little parade around the chamber.',
  sowing: 'is down in the soil, pressing a fresh seed into a plot.',
  gardening: 'is minding the garden, waiting for a bare patch to sow.',
  heading_to_garden: 'is making the long walk over to tend the garden next door.',
  tending_brood: 'is fussing over the eggs and grubs in the nursery.',
  feeding_queen: 'is bringing the queen her royal ration.',
  crafting: 'is lost in the muse, crafting a little keepsake.',
  mourning: 'is standing vigil for a friend who has passed.',
  clearing: 'is quietly clearing away what the colony can reclaim.',
  away: 'is off in another chamber, out of sight for now.',
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
          activity: l.activity,
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
        onTogglePin={() => togglePin(selectedId, char?.name)}
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
              {(() => {
                // Epithets make them memorable at a glance ("Bramble the Bold")
                const ep = epithet(c.traits, c.personality);
                return ep && (
                  <span style={{ fontWeight: 400, fontStyle: 'italic', color: palette.dimText }}>
                    {' '}{ep}
                  </span>
                );
              })()}
              {c.died_unix && <span style={{ fontSize: SIZES.xs, color: palette.dimText, marginLeft: 6 }}>(deceased)</span>}
            </div>
            <div style={{ fontSize: SIZES.sm, color: palette.dimText }}>
              {deriveRoleTag(c.personality, c.role, c.traits)}
              {c.traits.length > 0 && ` \u00b7 ${c.traits.map(traitLabel).join(', ')}`}
            </div>
          </div>
          <button
            onClick={(e) => { e.stopPropagation(); togglePin(c.id, c.name); }}
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
  // LAN detail is freshest but unreachable from the house WiFi (queen sits
  // behind the repeater NAT) — the VPS snapshot roster carries activity too
  // since v199, so fall back to it rather than hiding the card.
  const activity = detail?.activity ?? char.activity;
  const isDeceased = !!char.died_unix || !!detail?.died_unix;
  const { colonyId, events: colonyEvents, snapshot } = useColony();
  const [renaming, setRenaming] = useState(false);

  // History refines the epithet (parade-leaders, keen eyes) beyond what
  // traits + personality alone can tell
  const ep = useMemo(() => epithet(char.traits, personality, events),
    [char.traits, personality, events]);

  const rosterNames = useMemo(() => {
    const map = new Map<number, string>();
    if (snapshot?.lilguys) for (const l of snapshot.lilguys) map.set(l.id, l.name);
    return map;
  }, [snapshot]);

  const story = useMemo(() => buildLifeStory({
    id: char.id,
    name: char.name,
    founder: char.founder,
    died_unix: char.died_unix || detail?.died_unix || undefined,
    age_days: detail?.age_days ?? char.age_days,
    born_unix: detail?.born_unix,
    tended_by_name: detail?.tended_by?.name,
  }, events, rosterNames, colonyEvents),
    [char, detail, events, rosterNames, colonyEvents]);

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
        {ep && (
          <span style={{ fontWeight: 400, fontStyle: 'italic', fontSize: SIZES.lg, color: palette.dimText }}>
            {' '}{ep}
          </span>
        )}
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
            ageDays={detail?.age_days ?? char.age_days}
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
          {typeof char.needs?.loneliness === 'number' && (
            <div style={{ marginTop: 10 }}>
              <div style={{ display: 'flex', justifyContent: 'space-between',
                            fontSize: SIZES.xs, color: palette.dimText, marginBottom: 3 }}>
                <span>Loneliness</span>
                <span>{Math.round(char.needs.loneliness * 100)}%</span>
              </div>
              <div style={{ height: 6, borderRadius: 3, background: HIVE.parchment, overflow: 'hidden' }}>
                <div style={{
                  width: `${Math.round(Math.min(1, char.needs.loneliness) * 100)}%`,
                  height: '100%',
                  background: char.needs.loneliness >= 0.85 ? HIVE.alert
                            : char.needs.loneliness >= 0.55 ? HIVE.accent
                            : HIVE.leafGreen,
                }} />
              </div>
            </div>
          )}
        </Card>
      )}

      {/* Currently doing (live conkers only; LAN detail or VPS snapshot) */}
      {!isDeceased && activity && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText,
                        textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
            Right now
          </div>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
            <span style={{ fontSize: 22 }}>{ACTIVITY_EMOJI[activity]}</span>
            <div>
              <div style={{ fontSize: SIZES.base, color: palette.text }}>
                {ACTIVITY_TITLE[activity]}
              </div>
              <div style={{ fontSize: SIZES.xs, color: palette.dimText }}>
                {char.name} {ACTIVITY_BLURB[activity]}
              </div>
            </div>
          </div>
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
            // doesn't tell you who they're bonded to"). Only mutual
            // (reciprocated) bonds are "best friends" — one-way ones are soft
            // spots, matching the web + the bond list below (feedback: "best
            // friends are not best friends").
            let desc = info?.desc;
            if (t === 'bonded') {
              const joinNames = (bs: typeof bonds) => {
                const names = bs.map(b => b.to.name);
                return names.length === 1
                  ? names[0]
                  : `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
              };
              const best = bonds.filter(b => b.reciprocated);
              const soft = bonds.filter(b => !b.reciprocated);
              const parts: string[] = [];
              if (best.length > 0) parts.push(`Best friends with ${joinNames(best)}.`);
              if (soft.length > 0) parts.push(`Has a soft spot for ${joinNames(soft)}.`);
              if (parts.length > 0) desc = parts.join(' ');
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

      {/* Life Story — their history told as a biography, oldest first */}
      {story.length > 0 && (
        <Card style={{ background: palette.cardBg }}>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: palette.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
            Life Story
          </div>
          {story.map((s, i) => (
            <div key={i} style={{ display: 'flex', gap: 8, padding: '4px 0', alignItems: 'baseline' }}>
              <span style={{ minWidth: 22, textAlign: 'center' }}>{s.icon}</span>
              <div style={{ flex: 1 }}>
                <span style={{ fontSize: SIZES.sm, color: palette.text }}>{s.text}</span>
                <span style={{ fontSize: SIZES.xs, color: palette.dimText, marginLeft: 6 }}>
                  {new Date(s.unix * 1000).toLocaleDateString(undefined, { month: 'short', day: 'numeric' })}
                </span>
              </div>
            </div>
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

// (EventRow / formatEventType removed — the Life Story card narrativizes
// per-conker history via data/biography.ts instead of bare event labels.)
