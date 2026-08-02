/* Shop — spend the bugs she's caught on her room, and on things for her to carry.
 *
 * The currency is critters she found herself, so the Field Guide loop finally
 * pays for something (keeper feedback #33: "cookie clicker works because there
 * are mini goals/progression — currently missing"). Everything here is
 * cosmetic: identity, not power.
 *
 * You pay ONCE. Owned keepsakes swap freely — she carries one at a time, so
 * without ownership buying a second would quietly cost you the first, and
 * swapping back would charge you again.
 *
 * The catalogue is read from the SIM (localModule.shopItems), never duplicated
 * here — a price drifting between the two would surface only as a keeper being
 * charged the wrong amount.
 */
import { useEffect, useState } from 'react';
import { localModule } from '../localModule';
import { tintParams } from '../components/ConkerSprite';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';

interface ShopItem { id: number; name: string; price: number; tint: number; wear: number }

const SECTIONS: [string, boolean][] = [['For her room', false], ['For her to carry', true]];

// Her room's floor colour (feedback #59: "be able to change the background
// color"). Free — this isn't earned, it's just how her room looks.
const ROOM_COLOURS: [string, [number, number, number]][] = [
  ['Sand',    [216, 190, 140]],
  ['Moss',    [150, 176, 124]],
  ['Rose',    [214, 160, 168]],
  ['Sky',     [150, 178, 206]],
  ['Heather', [178, 156, 200]],
  ['Ember',   [214, 158, 116]],
  ['Slate',   [156, 160, 168]],
];

export function Shop() {
  const [items, setItems] = useState<ShopItem[]>([]);
  const [bugs, setBugs] = useState(0);
  const [owned, setOwned] = useState(0);
  const [worn, setWorn] = useState(0);
  const [flash, setFlash] = useState<string | null>(null);

  useEffect(() => {
    localModule.start().catch(() => {});
    const poll = window.setInterval(() => {
      setBugs(localModule.bugs());
      setOwned(localModule.ownedItems());
      setWorn(localModule.wornItem());
      setItems((prev) => (prev.length ? prev : localModule.shopItems()));
    }, 500);
    return () => clearInterval(poll);
  }, []);

  const say = (msg: string) => { setFlash(msg); window.setTimeout(() => setFlash(null), 2600); };

  const act = (it: ShopItem, isOwned: boolean) => {
    if (localModule.buyDecor(it.id)) {
      setBugs(localModule.bugs());
      setOwned(localModule.ownedItems());
      setWorn(localModule.wornItem());
      say(it.wear
        ? `She's carrying the ${it.name.toLowerCase()} ✨`
        : `${it.name} is in her room ✨`);
    } else if (isOwned) {
      say(`${it.name} is already in her room.`);
    } else {
      say(`Not enough bugs for the ${it.name.toLowerCase()} yet.`);
    }
  };

  const takeOff = () => {
    if (localModule.unequip()) { setWorn(0); say('She set it down.'); }
  };

  return (
    <div style={{ padding: '16px', display: 'flex', flexDirection: 'column', gap: 14 }}>
      <div>
        <h1 style={{ margin: 0, fontSize: SIZES.xl, color: HIVE.ink }}>Shop</h1>
        <div style={{ color: HIVE.dimText, fontSize: SIZES.sm, marginTop: 2 }}>
          Every critter she catches goes in the jar. Spend them on her room, or on
          something for her to carry.
        </div>
      </div>

      <div style={{
        display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        background: '#F5ECDD', borderRadius: 12, padding: '10px 14px',
      }}>
        <span style={{ fontSize: SIZES.sm, color: HIVE.soil }}>🐛 Bugs in the jar</span>
        <span style={{ fontSize: SIZES.lg, fontWeight: 700, color: HIVE.ink, fontVariantNumeric: 'tabular-nums' }}>
          {bugs}
        </span>
      </div>

      {flash && (
        <div style={{ fontSize: SIZES.sm, color: HIVE.soil, textAlign: 'center' }}>{flash}</div>
      )}

      {/* Room colour — free, and deliberately outside the bug economy */}
      <div>
        <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: HIVE.dimText,
                      textTransform: 'uppercase', letterSpacing: 1, marginBottom: 6 }}>
          Her room&rsquo;s colour
        </div>
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 8 }}>
          {ROOM_COLOURS.map(([name, [r, g, b]]) => (
            <button
              key={name}
              title={name}
              aria-label={name}
              onClick={() => { localModule.setRoomTint(r, g, b); say(`Her room is ${name.toLowerCase()} now.`); }}
              style={{
                width: 32, height: 32, borderRadius: '50%', cursor: 'pointer',
                background: `rgb(${r},${g},${b})`, border: `2px solid ${HIVE.parchment}`,
              }}
            />
          ))}
        </div>
      </div>

      {items.length === 0 ? (
        <div style={{ fontSize: SIZES.sm, color: HIVE.dimText, textAlign: 'center', padding: '18px 0' }}>
          The shop opens once she&rsquo;s settled in.
        </div>
      ) : (
        <div style={{ display: 'grid', gap: 10 }}>
          {SECTIONS.map(([label, wanted]) => {
            const group = items.filter((i) => (i.wear !== 0) === wanted);
            if (!group.length) return null;
            return (
              <div key={label} style={{ display: 'grid', gap: 10 }}>
                <div style={{ display: 'flex', alignItems: 'baseline',
                              justifyContent: 'space-between', marginTop: 4 }}>
                  <span style={{ fontSize: SIZES.xs, fontWeight: 600, color: HIVE.dimText,
                                 textTransform: 'uppercase', letterSpacing: 1 }}>
                    {label}
                  </span>
                  {wanted && worn !== 0 && (
                    <button
                      onClick={takeOff}
                      style={{ background: 'none', border: `1px solid ${HIVE.sand}`, borderRadius: 8,
                               padding: '3px 9px', color: HIVE.soil, fontSize: SIZES.xs, cursor: 'pointer' }}
                    >
                      Put it down
                    </button>
                  )}
                </div>
                {group.map((it) => {
                  const isOwned = (owned & (1 << it.id)) !== 0;
                  const isWorn = it.wear !== 0 && worn === it.wear;
                  const afford = isOwned || bugs >= it.price;
                  const done = isWorn || (isOwned && it.wear === 0);
                  const { hue, saturate, bright } = tintParams(it.tint);
                  return (
                    <div key={it.id} style={{
                      display: 'flex', alignItems: 'center', gap: 12,
                      background: isWorn ? '#F1E7D2' : HIVE.cream,
                      border: `1px solid ${isWorn ? HIVE.leafGreen : HIVE.sand}`,
                      borderRadius: 12, padding: '10px 12px', opacity: afford ? 1 : 0.6,
                    }}>
                      <div style={{
                        width: 34, height: 34, borderRadius: 9, flexShrink: 0,
                        background: `hsl(${Math.round(hue)}, ${Math.round(Math.min(85, saturate * 45))}%, ${Math.round(Math.min(68, 42 * bright))}%)`,
                      }} />
                      <div style={{ flex: 1, minWidth: 0 }}>
                        <div style={{ fontSize: SIZES.base, fontWeight: 600, color: HIVE.ink }}>{it.name}</div>
                        <div style={{ fontSize: SIZES.xs, color: HIVE.dimText }}>
                          {isOwned ? 'Yours' : `🐛 ${it.price}`}
                        </div>
                      </div>
                      <button
                        onClick={() => act(it, isOwned)}
                        disabled={!afford || done}
                        style={{
                          padding: '7px 14px', borderRadius: 10, flexShrink: 0, minWidth: 78,
                          border: `1px solid ${isWorn ? HIVE.leafGreen : afford && !done ? HIVE.green : HIVE.sand}`,
                          background: !done && afford ? HIVE.green : 'transparent',
                          color: isWorn ? HIVE.leafGreen : !done && afford ? HIVE.white : HIVE.dimText,
                          fontSize: SIZES.sm, fontWeight: 600,
                          cursor: afford && !done ? 'pointer' : 'default',
                        }}
                      >
                        {isWorn ? 'Carrying'
                          : isOwned ? (it.wear ? 'Carry' : 'In her room')
                          : afford ? 'Buy' : `${it.price - bugs} more`}
                      </button>
                    </div>
                  );
                })}
              </div>
            );
          })}
        </div>
      )}

      <div style={{ fontSize: SIZES.xs, color: HIVE.dimText, textAlign: 'center', marginTop: 4 }}>
        Rarer visitors are worth more — a ladybird counts for four beetles.
        You only pay once: anything of hers can be picked back up for nothing,
        and it all goes with her if she&rsquo;s ever crowned.
      </div>
    </div>
  );
}
