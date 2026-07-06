/* Module — this install's own live queen module.
 *
 * Loads the firmware (sim + renderer) compiled to WASM (public/hive/) and runs
 * it on a canvas: the phone literally becomes a queen module. Same code as the
 * hardware, so it's feature-complete and updates in lockstep (rebuild hive.wasm
 * from firmware/). The colony persists per-install in localStorage.
 *
 * Not yet wired to the VPS — this is the local, self-contained module. VPS
 * enrolment (so it appears in the other tabs + feedback) is the next step.
 */
import { useEffect, useRef, useState } from 'react';

declare global {
  interface Window { createHiveModule?: (opts?: Record<string, unknown>) => Promise<any>; }
}

const SAVE_KEY = 'hive_module_colony_v1';
const BASE = import.meta.env.BASE_URL;               // '/app/' in prod, '/' in dev
const SCRIPT_URL = `${BASE}hive/hive.js`;

// Load the emscripten glue script once; resolves the createHiveModule factory.
let scriptPromise: Promise<void> | null = null;
function loadHiveScript(): Promise<void> {
  if (window.createHiveModule) return Promise.resolve();
  if (scriptPromise) return scriptPromise;
  scriptPromise = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = SCRIPT_URL;
    s.async = true;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error('failed to load hive.js'));
    document.head.appendChild(s);
  });
  return scriptPromise;
}

export default function Module() {
  const canvasRef = useRef<HTMLCanvasElement | null>(null);
  const [status, setStatus] = useState<'loading' | 'running' | 'error'>('loading');
  const [hud, setHud] = useState<{ fps: number; pop: number }>({ fps: 0, pop: 0 });

  useEffect(() => {
    let raf = 0;
    let disposed = false;
    let saveNow: (() => void) | null = null;

    (async () => {
      try {
        await loadHiveScript();
        const M = await window.createHiveModule!({
          // Resolve hive.wasm next to hive.js under the app base path.
          locateFile: (p: string) => `${BASE}hive/${p}`,
        });
        if (disposed) return;

        const boot   = M.cwrap('host_boot', null, ['number']);
        const step   = M.cwrap('host_step', null, ['number']);
        const fb      = M.cwrap('host_fb', 'number', []);
        const w       = M.cwrap('host_w', 'number', []);
        const h       = M.cwrap('host_h', 'number', []);
        const feed    = M.cwrap('host_feed', null, ['number', 'number']);
        const conkers = M.cwrap('host_conkers', 'number', []);
        const statePtr = M.cwrap('host_state_ptr', 'number', []);
        const stateCap = M.cwrap('host_state_cap', 'number', []);
        const serialize = M.cwrap('host_serialize', 'number', []);

        // ---- persistence (this install's colony) ----
        const load = (): number => {
          try {
            const b64 = localStorage.getItem(SAVE_KEY);
            if (!b64) return 0;
            const bin = atob(b64);
            if (bin.length > stateCap()) return 0;
            const dst = M.HEAPU8.subarray(statePtr(), statePtr() + bin.length);
            for (let i = 0; i < bin.length; i++) dst[i] = bin.charCodeAt(i);
            return bin.length;
          } catch { return 0; }
        };
        saveNow = () => {
          try {
            const len = serialize();
            if (!len) return;
            const src = M.HEAPU8.subarray(statePtr(), statePtr() + len);
            let bin = '';
            for (let i = 0; i < len; i++) bin += String.fromCharCode(src[i]);
            localStorage.setItem(SAVE_KEY, btoa(bin));
          } catch { /* quota / private mode */ }
        };

        boot(load());
        const W = w(), H = h();
        const canvas = canvasRef.current!;
        canvas.width = W; canvas.height = H;
        const ctx = canvas.getContext('2d', { alpha: false })!;
        const image = ctx.createImageData(W, H);
        const rgba = image.data;
        setStatus('running');

        // Tap-to-feed
        const onDown = (e: PointerEvent) => {
          e.preventDefault();
          const r = canvas.getBoundingClientRect();
          feed(Math.floor((e.clientX - r.left) / r.width * W),
               Math.floor((e.clientY - r.top) / r.height * H));
        };
        canvas.addEventListener('pointerdown', onDown);

        const blit = () => {
          const p = fb();
          const src = M.HEAPU16.subarray(p >> 1, (p >> 1) + W * H);
          for (let i = 0, o = 0; i < W * H; i++, o += 4) {
            const v = src[i];
            const r5 = (v >> 11) & 0x1f, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
            rgba[o] = (r5 << 3) | (r5 >> 2);
            rgba[o + 1] = (g6 << 2) | (g6 >> 4);
            rgba[o + 2] = (b5 << 3) | (b5 >> 2);
            rgba[o + 3] = 255;
          }
          ctx.putImageData(image, 0, 0);
        };

        let last = performance.now();
        let fpsEMA = 60, hudT = 0, saveT = 0;
        const loop = (now: number) => {
          if (disposed) return;
          const dt = now - last; last = now;
          step(dt);
          blit();
          fpsEMA += ((1000 / Math.max(dt, 1)) - fpsEMA) * 0.1;
          if ((hudT += dt) > 500) { hudT = 0; setHud({ fps: Math.round(fpsEMA), pop: conkers() }); }
          if ((saveT += dt) > 5000) { saveT = 0; saveNow!(); }
          raf = requestAnimationFrame(loop);
        };
        raf = requestAnimationFrame(loop);
      } catch (e) {
        console.error('[module]', e);
        if (!disposed) setStatus('error');
      }
    })();

    return () => {
      disposed = true;
      if (raf) cancelAnimationFrame(raf);
      if (saveNow) saveNow();   // save on tab-switch / unmount
    };
  }, []);

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center',
                  gap: 8, padding: '8px 0' }}>
      <canvas
        ref={canvasRef}
        width={480}
        height={320}
        style={{
          imageRendering: 'pixelated',
          width: 'min(100%, calc((100vh - 220px) * 1.5))',
          aspectRatio: '480 / 320',
          borderRadius: 10,
          touchAction: 'none',
          boxShadow: '0 6px 30px rgba(0,0,0,.4)',
          background: '#0b0d0a',
        }}
      />
      <div style={{ fontSize: 12, opacity: 0.55, fontFamily: 'ui-monospace, Consolas, monospace' }}>
        {status === 'loading' && 'warming up the colony…'}
        {status === 'error'   && 'could not load the module'}
        {status === 'running' && `your colony · tap to feed · ${hud.fps} fps · ${hud.pop} conkers`}
      </div>
    </div>
  );
}
