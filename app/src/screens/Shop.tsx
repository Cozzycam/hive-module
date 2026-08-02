/* Shop — spend the bugs she's caught on things for her room.
 *
 * The currency is critters she found herself, so the Field Guide loop finally
 * pays for something (keeper feedback #33: "cookie clicker works because there
 * are mini goals/progression — currently missing"). Everything here is
 * cosmetic: identity, not power.
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

// She carries one keepsake at a time, so buying another simply swaps it.
const SECTIONS: [string, boolean][] = [['For her room', false], ['For her to carry', true]];

export function Shop() {
  const [items, setItems] = useState<ShopItem[]>([]);
  const [bugs, setBugs] = useState(0);
  const [flash, setFlash] = useState<string | null>(null);

  useEffect(() => {
    localModule.start().catch(() => {});
    const poll = window.setInterval(() => {
      setBugs(localModule.bugs());
      setItems((prev) => (prev.length ? prev : localModule.shopItems()));
    }, 500);
    return () => clearInterval(poll);
  }, []);

  const buy = (it: ShopItem) => {
    if (localModule.buyDecor(it.id)) {
      setFlash(it.wear ? `She's carrying the ${it.name.toLowerCase()} ✨` : `${it.name} is in her room ✨`);
      setBugs(localModule.bugs());
    } else {
      setFlash(`Not enough bugs for the ${it.name.toLowerCase()} yet.`);
    }
    window.setTimeout(() => setFlash(null), 2600);
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

      {items.length === 0 ? (
        <div style={{ fontSize: SIZES.sm, color: HIVE.dimText, textAlign: 'center', padding: '18px 0' }}>
          The shop opens once she's settled in.
        </div>
      ) : (
        <div style={{ display: 'grid', gap: 10 }}>
          {SECTIONS.map(([label, wanted]) => {
            const group = items.filter((i) => (i.wear !== 0) === wanted);
            if (!group.length) return null;
            return (
              <div key={label} style={{ display: 'grid', gap: 10 }}>
                <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: HIVE.dimText,
                              textTransform: 'uppercase', letterSpacing: 1, marginTop: 4 }}>
                  {label}
                </div>
                {group.map((it) => {
            const afford = bugs >= it.price;
            const { hue, saturate, bright } = tintParams(it.tint);
            return (
              <div key={it.id} style={{
                display: 'flex', alignItems: 'center', gap: 12,
                background: HIVE.cream, border: `1px solid ${HIVE.sand}`,
                borderRadius: 12, padding: '10px 12px', opacity: afford ? 1 : 0.6,
              }}>
                <div style={{
                  width: 34, height: 34, borderRadius: 9, flexShrink: 0,
                  background: `hsl(${Math.round(hue)}, ${Math.round(Math.min(85, saturate * 45))}%, ${Math.round(Math.min(68, 42 * bright))}%)`,
                }} />
                <div style={{ flex: 1, minWidth: 0 }}>
                  <div style={{ fontSize: SIZES.base, fontWeight: 600, color: HIVE.ink }}>{it.name}</div>
                  <div style={{ fontSize: SIZES.xs, color: HIVE.dimText }}>🐛 {it.price}</div>
                </div>
                <button
                  onClick={() => buy(it)}
                  disabled={!afford}
                  style={{
                    padding: '7px 14px', borderRadius: 10, flexShrink: 0,
                    border: `1px solid ${afford ? HIVE.green : HIVE.sand}`,
                    background: afford ? HIVE.green : 'transparent',
                    color: afford ? HIVE.white : HIVE.dimText,
                    fontSize: SIZES.sm, fontWeight: 600,
                    cursor: afford ? 'pointer' : 'default',
                  }}
                >
                  {afford ? 'Buy' : `${it.price - bugs} more`}
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
        Anything you buy stays for good, and goes with her if she's ever crowned.
        She carries one keepsake at a time — buying another just swaps it.
      </div>
    </div>
  );
}
