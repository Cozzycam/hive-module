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
// Fallback window, used only on an install still running a cached pre-v223 wasm
// that has no room exports. Otherwise the view IS her room, reported by the sim.
const CROP_W = 174, CROP_H = 232;

// A phone really only offers a tap and a tap-and-hold. So the tap is a TOOL:
// pick one here, then tap the glass. `cool` indexes Cfg::InteractKind
// (boop 0, ball 1, water 2) — -1 means the tool has no cooldown of its own.
type Tool = 'food' | 'boop' | 'ball' | 'water' | 'move';
const TOOLS: { id: Tool; icon: string; label: string; cool: number }[] = [
  { id: 'food',  icon: '🍎', label: 'Food',  cool: -1 },
  { id: 'boop',  icon: '👆', label: 'Boop',  cool: 0 },
  { id: 'ball',  icon: '🎾', label: 'Ball',  cool: 1 },
  { id: 'water', icon: '💧', label: 'Water', cool: 2 },
  { id: 'move',  icon: '✋', label: 'Move',  cool: -1 },
];

export function Her({ onNavigate }: { onNavigate?: (target: string) => void }) {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [status, setStatus] = useState<'loading' | 'running' | 'error'>('loading');
  const [stats, setStats] = useState<PrincessStats | null>(null);
  // Canvas size must be REACT STATE, not an imperative canvas.width assignment:
  // width/height are JSX props here, so every re-render (the stats poll, 4x a
  // second) resets them and silently undoes an imperative resize.
  const [canvasSize, setCanvasSize] = useState({ w: CROP_W, h: CROP_H });
  // A phone gives you a tap and a tap-and-hold, so the tap has to be switchable:
  // pick a tool, then tap the glass to use it (hold still gathers).
  const [tool, setTool] = useState<Tool>('food');
  const [cool, setCool] = useState<number[]>([0, 0, 0]);
  const [flowering, setFlowering] = useState(0);
  // Move tool: remember what we picked up so pointerup knows where it came from.
  const dragRef = useRef<{ x: number; y: number } | null>(null);
  // Fires once, ever: she's reached full bond.
  const [peak, setPeak] = useState<string | null>(null);   // her name when it fires
  // The view rect. Static: her room is fenced to exactly this, so everything is
  // on screen at once and nothing chases her about. Only falls back to a
  // follow-camera when the wasm predates the room.
  const viewRef = useRef({ x: (FBW - CROP_W) / 2, y: (FBH - CROP_H) / 2, w: CROP_W, h: CROP_H, fixed: false });

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
        const view = viewRef.current;
        if (!view.fixed) {
          const room = localModule.room();
          if (room) {
            view.x = room.x; view.y = room.y; view.w = room.w; view.h = room.h;
            view.fixed = true;
            setCanvasSize((s) => (s.w === room.w && s.h === room.h ? s : { w: room.w, h: room.h }));
          } else {
            // Legacy wasm: keep the old follow-camera so the Nest still works.
            const s = localModule.princessStats();
            if (s) {
              const tx = clampC(s.px - view.w / 2, FBW - view.w);
              const ty = clampC(s.py - view.h / 2, FBH - view.h);
              if (Math.abs(tx - view.x) > 50 || Math.abs(ty - view.y) > 50) { view.x = tx; view.y = ty; }
              else { view.x += (tx - view.x) * 0.12; view.y += (ty - view.y) * 0.12; }
            }
          }
        }
        ctx.drawImage(off, view.x, view.y, view.w, view.h, 0, 0, canvas.width, canvas.height);
      }
      raf = requestAnimationFrame(loop);
    };
    raf = requestAnimationFrame(loop);

    const poll = window.setInterval(() => {
      if (!cancelled) {
        setStats(localModule.princessStats());
        setCool([0, 1, 2].map((k) => localModule.interactReady(k)));
        setFlowering(localModule.flowering());
        if (localModule.bondPeakNew()) {
          const snap = localModule.colonySnapshot() as { lilguys?: { name?: string }[] } | null;
          setPeak(snap?.lilguys?.[0]?.name || 'She');
        }
      }
    }, 250);

    return () => { cancelled = true; cancelAnimationFrame(raf); clearInterval(poll); };
  }, []);

  // Tap the glass — map portrait-canvas coords through the crop to framebuffer
  // coords, then host_tap (drops food where you tap / boops her if you hit her).
  // Canvas point -> framebuffer coords. Exact now the view is static; with the
  // old lerping camera a tap mapped through a window still gliding, so it
  // landed slightly off.
  const toFb = (e: React.PointerEvent) => {
    const c = canvasRef.current; if (!c) return null;
    const r = c.getBoundingClientRect();
    const v = viewRef.current;
    return {
      fx: v.x + ((e.clientX - r.left) / r.width) * v.w,
      fy: v.y + ((e.clientY - r.top) / r.height) * v.h,
    };
  };

  const onDown = (e: React.PointerEvent) => {
    if (status !== 'running') return;
    const p = toFb(e); if (!p) return;
    if (tool === 'move') {
      // Only start a drag if a piece is actually under the finger, so a stray
      // tap on bare floor doesn't teleport something across the room.
      dragRef.current = localModule.decorAt(p.fx, p.fy) ? { x: p.fx, y: p.fy } : null;
      return;
    }
    if (tool === 'ball')       localModule.throwBallAt(p.fx, p.fy);
    else if (tool === 'water') localModule.water();
    else if (tool === 'boop')  localModule.boopFb(p.fx, p.fy);  // a miss does nothing
    else                       localModule.tapFb(p.fx, p.fy);   // food, or boop if you hit her
  };

  const onUp = (e: React.PointerEvent) => {
    const from = dragRef.current;
    dragRef.current = null;
    if (tool !== 'move' || !from) return;
    const p = toFb(e); if (!p) return;
    localModule.moveDecor(from.x, from.y, p.fx, p.fy);   // the sim snaps it to a cell
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
    : flowering > 0 && !dormant ? '🌸 She’s flowering — watered and pleased with herself.'
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
                    background: '#12100e', boxShadow: '0 8px 28px rgba(0,0,0,0.25)',
                    position: 'relative' }}>
        {peak !== null && (
          <div
            onClick={() => setPeak(null)}
            style={{
              position: 'absolute', inset: 0, zIndex: 2, cursor: 'pointer',
              display: 'flex', flexDirection: 'column', alignItems: 'center',
              justifyContent: 'center', gap: 6, textAlign: 'center',
              background: 'rgba(24,18,14,0.55)', color: '#FFF4E2', padding: '0 18px',
            }}
          >
            <div style={{ fontSize: 34 }}>🌸</div>
            <div style={{ fontSize: SIZES.lg, fontWeight: 700 }}>
              {peak === 'She' ? 'She’s a big fan of you' : `${peak} is a big fan of you`}
            </div>
            <div style={{ fontSize: SIZES.sm, opacity: 0.9 }}>
              Number one fan. She’ll carry that with her — if she’s ever crowned,
              her colony will remember she was loved.
            </div>
            <div style={{ fontSize: SIZES.xs, opacity: 0.7, marginTop: 4 }}>tap to close</div>
          </div>
        )}
        <canvas
          ref={canvasRef}
          width={canvasSize.w}
          height={canvasSize.h}
          onPointerDown={onDown}
          onPointerUp={onUp}
          onPointerCancel={() => { dragRef.current = null; }}
          style={{ width: '100%', aspectRatio: `${canvasSize.w} / ${canvasSize.h}`, display: 'block',
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

      {/* tools — the tap is whichever of these is selected */}
      <div style={{ width: '100%', maxWidth: 320, display: 'flex', gap: 6 }}>
        {TOOLS.map((t) => {
          const active = tool === t.id;
          const resting = t.cool >= 0 && (cool[t.cool] ?? 0) > 0;
          return (
            <button
              key={t.id}
              onClick={() => setTool(t.id)}
              disabled={status !== 'running'}
              title={resting ? 'Still works — just won’t lift her spirits again yet' : t.label}
              style={{
                flex: 1, padding: '9px 0', borderRadius: 12, cursor: 'pointer',
                border: `1px solid ${active ? HIVE.green : HIVE.sand}`,
                background: active ? HIVE.green : HIVE.cream,
                color: active ? HIVE.white : HIVE.soil,
                fontSize: SIZES.xs, fontWeight: 600, lineHeight: 1.5,
                opacity: resting && !active ? 0.65 : 1,
              }}
            >
              <div style={{ fontSize: 18 }}>{t.icon}</div>
              {t.label}{resting ? ' ·' : ''}
            </button>
          );
        })}
      </div>

      <div style={{ fontSize: SIZES.xs, color: HIVE.dimText, textAlign: 'center', maxWidth: 300 }}>
        {tool === 'food'  && 'Tap the glass to drop food — she toddles over and eats it. Tap her directly for a boop.'}
        {tool === 'boop'  && 'Tap her to say hello.'}
        {tool === 'ball'  && 'Tap anywhere to throw the ball there — she’ll run it down and knock it on.'}
        {tool === 'water' && 'Tap to water her. The bud on her head opens into a flower for a few hours.'}
        {tool === 'move'  && 'Drag anything in her room to rearrange it — it snaps to the floor where you let go.'}
        {' '}
        A dot means she’s had her fill of that one for now — it still works, and still
        builds your bond, it just won’t lift her spirits again until she’s missed it.
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
