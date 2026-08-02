/* About — the tamagotchi "About her" profile (one creature).
 *
 * Reads the princess straight from the in-process module (localModule) so it
 * works with no VPS: her name (+ rename), what she's up to right now, how grown
 * she is, your bond, and her personality. Traits/keepsake fill in later once she
 * earns them (they come from her event history).
 */
import { useEffect, useState } from 'react';
import { localModule, resetColonyIdentity, AUTOSTART_KEY, WIPE_KEY, LAST_ACTIVE_KEY, type PrincessStats } from '../localModule';
import { clearConnection } from '../api/client';
import { PersonalityPetals } from '../components/PersonalityPetals';
import { tintParams } from '../components/ConkerSprite';
import type { Personality } from '../api/types';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';

interface Lilguy {
  id: number; name: string; age_days?: number; activity?: string; mood?: number;
  personality?: Record<string, number>; tint_seed?: number;
}

// A spread of colours to choose from (feedback #43/#49). Rather than hardcode
// magic seeds, walk the seed space and keep the ones whose resulting hues are
// furthest apart — so the swatches are always as distinct as the sim allows,
// and they show her ACTUAL colour because they use the firmware's derivation.
const PALETTE_SEEDS: number[] = (() => {
  const picked: { seed: number; hue: number }[] = [];
  for (let seed = 1; seed <= 255; seed++) {
    const { hue } = tintParams(seed);
    if (picked.every((p) => Math.abs(((p.hue - hue + 540) % 360) - 180) > 32)) {
      picked.push({ seed, hue });
    }
    if (picked.length >= 8) break;
  }
  return picked.map((p) => p.seed);
})();

const ACTIVITY_TEXT: Record<string, string> = {
  idling: 'pottering about', sleeping: 'fast asleep', napping: 'having a nap',
  seeking_company: 'looking for a friend', foraging: 'out foraging',
  carrying_food: 'carrying food home', heading_home: 'heading home', eating: 'having a bite',
  chasing_firefly: 'chasing a firefly', playing: 'playing', crafting: 'making something',
  gardening: 'minding the garden', sowing: 'sowing a seed', mourning: 'in mourning',
  away: 'out of sight', tending_brood: 'tending the brood', feeding_queen: 'feeding the queen',
};

export function About() {
  const [guy, setGuy] = useState<Lilguy | null>(null);
  const [stats, setStats] = useState<PrincessStats | null>(null);

  useEffect(() => {
    localModule.start().catch(() => {});
    const poll = window.setInterval(() => {
      const snap = localModule.colonySnapshot();
      const l = (snap?.lilguys as Lilguy[] | undefined)?.[0] ?? null;
      setGuy(l);
      setStats(localModule.princessStats());
    }, 500);
    return () => clearInterval(poll);
  }, []);

  const rename = () => {
    if (!guy) return;
    const next = window.prompt(`New name for ${guy.name} (letters, max 15):`, guy.name);
    if (next && /^[A-Za-z]{1,15}$/.test(next.trim())) localModule.renameConker(guy.id, next.trim());
  };

  // Start over with a fresh egg. Mint a new identity (a new colony_id reseeds the
  // sim, so a genuinely different Conker), flag the persisted colony for a wipe and
  // the next load to auto-hatch, drop the old connection, and reload. Same sequence
  // as Settings' "Start a fresh colony" — the only reset path tamagotchi mode has.
  const hatchNew = () => {
    const who = guy?.name ? guy.name : 'this little one';
    if (!window.confirm(
      `Hatch a new Conker? ${who} — and everything you’ve raised together — will be gone for good.`)) return;
    resetColonyIdentity();
    localStorage.setItem(AUTOSTART_KEY, '1');   // Empty.tsx auto-hatches the new egg on load
    localStorage.setItem(WIPE_KEY, '1');        // clear the persisted colony so it hatches fresh, not restores
    localStorage.removeItem(LAST_ACTIVE_KEY);   // no catch-up from the old egg's clock
    clearConnection();
    window.location.reload();
  };

  const hatchNewSection = (
    <div style={{ marginTop: 24, paddingTop: 16, borderTop: `1px solid ${HIVE.parchment}`, textAlign: 'center' }}>
      <button
        onClick={hatchNew}
        style={{
          background: 'none', border: `1px solid ${HIVE.sand}`, borderRadius: 20,
          padding: '8px 18px', color: HIVE.dimText, fontSize: SIZES.xs, cursor: 'pointer',
        }}
      >
        {'\u{1F95A}'} Hatch a new Conker
      </button>
      <div style={{ fontSize: SIZES.xs, color: HIVE.dimText, marginTop: 6, opacity: 0.85 }}>
        Start over with a fresh egg — this one won’t come back.
      </div>
    </div>
  );

  if (!guy) {
    return (
      <div style={{ padding: '32px 16px', textAlign: 'center', color: HIVE.dimText, fontSize: SIZES.base }}>
        {stats?.hatched === false ? 'She’s still an egg — she’ll be here once she hatches. 🥚' : 'Warming up…'}
        {hatchNewSection}
      </div>
    );
  }

  const ageDays = guy.age_days ?? 0;
  const ageStr = ageDays < 1 ? `${Math.round(ageDays * 24)}h old` : `${ageDays.toFixed(1)} days old`;
  const activity = ACTIVITY_TEXT[guy.activity || 'idling'] || (guy.activity || '').replace(/_/g, ' ');

  return (
    <div style={{ padding: '16px', display: 'flex', flexDirection: 'column', gap: 16 }}>
      {/* name + rename — the whole name is the button (feedback #15: the edit
          affordance was a faint outline nobody found) */}
      <div style={{ display: 'flex', alignItems: 'baseline', justifyContent: 'space-between', gap: 8 }}>
        <button
          onClick={rename}
          title="Tap to rename her"
          style={{ background: 'none', border: 'none', padding: 0, textAlign: 'left',
                   cursor: 'pointer', display: 'flex', alignItems: 'baseline', gap: 8 }}
        >
          <h1 style={{ margin: 0, fontSize: SIZES.xl, color: HIVE.ink,
                       textDecoration: 'underline', textDecorationStyle: 'dotted',
                       textDecorationColor: HIVE.sand, textUnderlineOffset: 5 }}>
            {guy.name}
          </h1>
          <span style={{ fontSize: SIZES.sm, color: HIVE.dimText }}>✎</span>
        </button>
        <button onClick={rename} style={{ background: 'none', border: `1px solid ${HIVE.sand}`, borderRadius: 8,
          padding: '5px 10px', color: HIVE.soil, fontSize: SIZES.xs, cursor: 'pointer', flexShrink: 0 }}>Rename</button>
      </div>
      <div style={{ marginTop: -8, color: HIVE.dimText, fontSize: SIZES.sm }}>
        {ageStr} · right now she’s {activity}.
      </div>

      {/* her colour (feedback #43/#49) — cosmetic only: identity, not power */}
      <div>
        <div style={{ fontSize: SIZES.sm, fontWeight: 600, color: HIVE.soil, marginBottom: 6 }}>
          Her colour
        </div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
          {PALETTE_SEEDS.map((seed) => {
            const { hue, saturate, bright } = tintParams(seed);
            const chosen = (guy.tint_seed ?? 0) === seed;
            return (
              <button
                key={seed}
                onClick={() => localModule.tintConker(guy.id, seed)}
                aria-label={`Colour ${seed}`}
                style={{
                  width: 34, height: 34, borderRadius: '50%', cursor: 'pointer',
                  background: `hsl(${Math.round(hue)}, ${Math.round(Math.min(85, saturate * 45))}%, ${Math.round(Math.min(68, 42 * bright))}%)`,
                  border: chosen ? `3px solid ${HIVE.ink}` : `2px solid ${HIVE.parchment}`,
                  boxShadow: chosen ? '0 0 0 2px rgba(0,0,0,0.06)' : 'none',
                }}
              />
            );
          })}
        </div>
        <div style={{ fontSize: SIZES.xs, color: HIVE.dimText, marginTop: 6 }}>
          Pick her colour — it changes how she looks, and nothing else about her.
        </div>
      </div>

      {/* growing up + your bond */}
      <div style={{ display: 'grid', gap: 8 }}>
        <StatBar label="Growing up" value={Math.round((stats?.maturity ?? 0) * 100)} color={HIVE.leafGreen} suffix="%" />
        <StatBar label="Your bond"  value={Math.round(stats?.bond ?? 0)} color="#c0568f" suffix="%" />
      </div>

      {/* personality — the same radar/petals chart the full app uses */}
      {guy.personality && (
        <div>
          <div style={{ fontSize: SIZES.sm, fontWeight: 600, color: HIVE.soil, marginBottom: 4, textAlign: 'center' }}>
            Personality
          </div>
          <div style={{ display: 'flex', justifyContent: 'center' }}>
            <PersonalityPetals personality={guy.personality as unknown as Personality} size={180} />
          </div>
        </div>
      )}

      <div style={{ fontSize: SIZES.xs, color: HIVE.dimText, textAlign: 'center', marginTop: 4 }}>
        Titles &amp; keepsakes appear here as she earns them.
      </div>

      {hatchNewSection}
    </div>
  );
}

function StatBar({ label, value, color, suffix }: { label: string; value: number; color: string; suffix?: string }) {
  return (
    <div style={{ display: 'grid', gridTemplateColumns: '84px 1fr 40px', alignItems: 'center', gap: 8 }}>
      <span style={{ fontSize: SIZES.xs, color: HIVE.dimText }}>{label}</span>
      <div style={{ height: 11, borderRadius: 6, background: HIVE.parchment, overflow: 'hidden' }}>
        <div style={{ height: '100%', width: `${Math.max(0, Math.min(100, value))}%`, background: color,
          borderRadius: 6, transition: 'width .25s ease' }} />
      </div>
      <span style={{ fontSize: SIZES.xs, color: HIVE.soil, textAlign: 'right', fontVariantNumeric: 'tabular-nums' }}>
        {value}{suffix || ''}
      </span>
    </div>
  );
}
