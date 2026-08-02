/* Her — the tamagotchi care screen (incubation mode).
 *
 * Shows the single raised princess in a zoomed portrait view (a crop of the
 * live WASM framebuffer, camera-following her), with her care loop: tap the
 * glass to drop food she toddles over and eats (which deepens your bond), tap
 * her to boop. Her needs / maturity / keeper-bond read straight from the
 * in-process module (localModule) — no VPS round-trip. When she's fully grown
 * she's "ready to bloom": the call to get a module and crown her lives here.
 */
import { useEffect, useRef, useState } from 'react';
import { localModule, type PrincessStats } from '../localModule';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';

const FBW = 480, FBH = 320;
const CROP_W = 174, CROP_H = 232;   // portrait window into the framebuffer (zoom)

export function Her({ onNavigate }: { onNavigate?: (target: string) => void }) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [status, setStatus] = useState<'loading' | 'running' | 'error'>('loading');
  const [stats, setStats] = useState<PrincessStats | null>(null);
  const camRef = useRef({ x: FBW / 2 - CROP_W / 2, y: FBH / 2 - CROP_H / 2 });

  // Boot the module + drive the portrait render loop.
  useEffect(() => {
    let raf = 0, cancelled = false;
    const off = document.createElement('canvas'); off.width = FBW; off.height = FBH;
    const offCtx = off.getContext('2d', { alpha: false })!;

    localModule.start()
      .then(() => { if (!cancelled) setStatus('running'); })
      .catch((e) => { console.error('[her]', e); if (!cancelled) setStatus('error'); });

    const clampC = (v: number, max: number) => v < 0 ? 0 : (v > max ? max : v);
    const loop = () => {
      if (cancelled) return;
      const canvas = canvasRef.current;
      const img = localModule.frameImage();
      if (canvas && img) {
        const ctx = canvas.getContext('2d', { alpha: false })!;
        ctx.imageSmoothingEnabled = false;
        offCtx.putImageData(img, 0, 0);
        const s = localModule.princessStats();
        const cam = camRef.current;
        if (s) {
          const tx = clampC(s.px - CROP_W / 2, FBW - CROP_W);
          const ty = clampC(s.py - CROP_H / 2, FBH - CROP_H);
          if (Math.abs(tx - cam.x) > 50 || Math.abs(ty - cam.y) > 50) { cam.x = tx; cam.y = ty; }
          else { cam.x += (tx - cam.x) * 0.12; cam.y += (ty - cam.y) * 0.12; }
        }
        ctx.drawImage(off, cam.x, cam.y, CROP_W, CROP_H, 0, 0, CROP_W, CROP_H);
      }
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);

    const poll = window.setInterval(() => {
      if (!cancelled) setStats(localModule.princessStats());
    }, 250);

    return () => { cancelled = true; cancelAnimationFrame(raf); clearInterval(poll); };
  }, []);

  // Tap the glass — map portrait-canvas coords through the crop to framebuffer
  // coords, then host_tap (drops food where you tap / boops her if you hit her).
  const onTap = (e: React.PointerEvent) => {
    if (status !== 'running') return;
    const c = canvasRef.current; if (!c) return;
    const r = c.getBoundingClientRect();
    const fx = camRef.current.x + ((e.clientX - r.left) / r.width) * CROP_W;
    const fy = camRef.current.y + ((e.clientY - r.top) / r.height) * CROP_H;
    localModule.tapFb(fx, fy);
  };

  const hatched = stats?.hatched ?? false;
  const dormant = stats?.dormant ?? false;
  const maturePct = Math.round((stats?.maturity ?? 0) * 100);
  const ready = maturePct >= 100;
  const dayN = stats?.founded ? Math.floor((Date.now() / 1000 - stats.founded) / 86400) + 1 : 1;

  const hungry = (stats?.hunger ?? 0) > 60;
  const playing = (stats?.ball ?? 0) > 0;
  const lonely = (stats?.social ?? 0) > 55;

  const statusText =
    !hatched ? '🥚 An egg. Keep it warm — she’ll hatch soon.'
    : dormant ? '😴 She’s curled up asleep. Drop some food to wake her.'
    : playing ? '🎾 She’s off after the ball!'
    : hungry  ? '🍎 She’s hungry — drop some food and she’ll toddle over.'
    : lonely  ? '🎾 She’s missing you — throw her the ball for a game.'
    : ready   ? '👑 Fully grown, and ready to bloom into a queen.'
    :           '🌱 Growing up. Keep her fed and she’ll thrive.';

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 12, padding: '10px 14px 20px' }}>
      {/* chrome */}
      <div style={{ width: '100%', maxWidth: 320, display: 'flex', justifyContent: 'space-between',
                    fontSize: SIZES.xs, color: HIVE.dimText, padding: '0 4px' }}>
        <span>{new Date().toTimeString().slice(0, 5)}</span>
        <span style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
          Day {dayN}
          <button
            onClick={() => onNavigate?.('settings')}
            style={{
              background: 'none', border: 'none', cursor: 'pointer',
              fontSize: 16, color: HIVE.dimText, padding: '2px 4px',
            }}
            aria-label="Settings"
          >
            &#9881;
          </button>
        </span>
      </div>

      {/* the glass */}
      <div style={{ width: '100%', maxWidth: 320, borderRadius: 18, overflow: 'hidden',
                    background: '#12100e', boxShadow: '0 8px 28px rgba(0,0,0,0.25)' }}>
        <canvas
          ref={canvasRef}
          width={CROP_W}
          height={CROP_H}
          onPointerDown={onTap}
          style={{ width: '100%', aspectRatio: `${CROP_W} / ${CROP_H}`, display: 'block',
                   imageRendering: 'pixelated', touchAction: 'none', cursor: 'pointer', background: '#12100e' }}
        />
      </div>

      <div style={{ fontSize: SIZES.sm, color: HIVE.soil, minHeight: '1.3em', textAlign: 'center' }}>
        {status === 'loading' ? 'warming up…' : status === 'error' ? 'could not start' : statusText}
      </div>

      {/* stat bars */}
      <div style={{ width: '100%', maxWidth: 320, display: 'grid', gap: 8 }}>
        <Bar label="Growing up" value={maturePct} color={HIVE.leafGreen} suffix="%" />
        <Bar label="Your bond" value={Math.round(stats?.bond ?? 0)} color="#c0568f" suffix="%" />
        <Bar label="Hunger"     value={Math.round(stats?.hunger ?? 0)} color="#c9873d" />
        <Bar label="Boredom"    value={Math.round(stats?.boredom ?? 0)} color="#9a86c0" />
        <Bar label="Loneliness" value={Math.round(stats?.social ?? 0)}  color="#9a86c0" />
        <Bar label="Tiredness"  value={Math.round(stats?.rest ?? 0)}    color="#9a86c0" />
      </div>

      {/* care */}
      <div style={{ width: '100%', maxWidth: 320, display: 'flex', gap: 8 }}>
        <button
          onClick={() => localModule.feedPrincess()}
          disabled={status !== 'running'}
          style={{ flex: 1, padding: '12px', borderRadius: 12, border: 'none',
                   background: HIVE.green, color: HIVE.white, fontSize: SIZES.base, fontWeight: 600,
                   cursor: 'pointer' }}
        >
          🍎 Drop food
        </button>
        <button
          onClick={() => localModule.throwBall()}
          disabled={status !== 'running' || !hatched}
          style={{ flex: 1, padding: '12px', borderRadius: 12, border: 'none',
                   background: playing ? HIVE.sand : '#c0568f', color: playing ? HIVE.soil : HIVE.white,
                   fontSize: SIZES.base, fontWeight: 600,
                   cursor: status === 'running' && hatched ? 'pointer' : 'default' }}
        >
          {playing ? '🎾 Chasing…' : '🎾 Throw a ball'}
        </button>
      </div>
      <div style={{ fontSize: SIZES.xs, color: HIVE.dimText, textAlign: 'center', maxWidth: 300 }}>
        Tap the glass to drop food where you tap — she toddles over and eats it, and being hand-fed is what
        deepens her bond. Tap her directly to give her a boop. Throw the ball and she’ll chase it down and
        knock it on a few times — playing together is the fastest way to grow close.
      </div>

      {/* Dev-only fast-forward — never shown in production */}
      {import.meta.env.DEV && (
        <div style={{ display: 'flex', flexWrap: 'wrap', gap: 6, justifyContent: 'center', marginTop: 2 }}>
          {([['+1h', 3600, true], ['+6h', 6 * 3600, true], ['neglect 2d', 2 * 86400, false],
             ['neglect 5d', 5 * 86400, false]] as const).map(([label, secs, fed]) => (
            <button key={label} onClick={() => localModule.warp(secs, fed)}
              style={{ fontSize: 11, padding: '5px 9px', borderRadius: 7, cursor: 'pointer',
                       border: `1px solid ${HIVE.sand}`, background: HIVE.parchment, color: HIVE.soil }}>
              {label}
            </button>
          ))}
        </div>
      )}

      {/* ready to bloom → coronation CTA */}
      {ready && (
        <div style={{ width: '100%', maxWidth: 320, marginTop: 4, padding: '14px 16px', borderRadius: 14,
                      background: HIVE.accent, color: HIVE.ink, textAlign: 'center' }}>
          <div style={{ fontSize: SIZES.base, fontWeight: 700 }}>She’s ready to become a queen 👑</div>
          <div style={{ fontSize: SIZES.sm, marginTop: 4, opacity: 0.85 }}>
            When you bring her home to a Hive Module, she’ll be crowned and found a colony of her own —
            same name, same colours, the personality you raised.
          </div>
          <button
            onClick={() => onNavigate?.('crown')}
            style={{ marginTop: 10, padding: '10px 18px', borderRadius: 10, border: 'none',
                     cursor: 'pointer', background: HIVE.ink, color: HIVE.white,
                     fontSize: SIZES.sm, fontWeight: 600 }}
          >
            👑 Begin her coronation
          </button>
        </div>
      )}
    </div>
  );
}

function Bar({ label, value, color, suffix }: { label: string; value: number; color: string; suffix?: string }) {
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
