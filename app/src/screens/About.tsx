/* About — the tamagotchi "About her" profile (one creature).
 *
 * Reads the princess straight from the in-process module (localModule) so it
 * works with no VPS: her name (+ rename), what she's up to right now, how grown
 * she is, your bond, and her personality. Traits/keepsake fill in later once she
 * earns them (they come from her event history).
 */
import { useEffect, useState } from 'react';
import { localModule, type PrincessStats } from '../localModule';
import { PersonalityPetals } from '../components/PersonalityPetals';
import type { Personality } from '../api/types';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';

interface Lilguy {
  id: number; name: string; age_days?: number; activity?: string; mood?: number;
  personality?: Record<string, number>;
}

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

  if (!guy) {
    return (
      <div style={{ padding: '32px 16px', textAlign: 'center', color: HIVE.dimText, fontSize: SIZES.base }}>
        {stats?.hatched === false ? 'She’s still an egg — she’ll be here once she hatches. 🥚' : 'Warming up…'}
      </div>
    );
  }

  const ageDays = guy.age_days ?? 0;
  const ageStr = ageDays < 1 ? `${Math.round(ageDays * 24)}h old` : `${ageDays.toFixed(1)} days old`;
  const activity = ACTIVITY_TEXT[guy.activity || 'idling'] || (guy.activity || '').replace(/_/g, ' ');

  return (
    <div style={{ padding: '16px', display: 'flex', flexDirection: 'column', gap: 16 }}>
      {/* name + rename */}
      <div style={{ display: 'flex', alignItems: 'baseline', justifyContent: 'space-between' }}>
        <h1 style={{ margin: 0, fontSize: SIZES.xl, color: HIVE.ink }}>{guy.name}</h1>
        <button onClick={rename} style={{ background: 'none', border: `1px solid ${HIVE.sand}`, borderRadius: 8,
          padding: '5px 10px', color: HIVE.soil, fontSize: SIZES.xs, cursor: 'pointer' }}>✎ Rename</button>
      </div>
      <div style={{ marginTop: -8, color: HIVE.dimText, fontSize: SIZES.sm }}>
        {ageStr} · right now she’s {activity}.
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
