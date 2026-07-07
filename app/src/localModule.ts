/* localModule — runs THIS install's queen module in the browser and pushes it
 * to the VPS on the same pipeline as hardware (module → VPS → app).
 *
 * A singleton that lives outside React, so the module keeps ticking + pushing
 * no matter which tab is showing (or none). The Module tab just displays its
 * canvas; the rest of the app reads the colony back from the VPS like any other.
 *
 * Each install owns ONE colony: a colony_id + secret minted once into
 * localStorage; first push TOFU-enrols, later pushes sign with the per-colony
 * secret (WebCrypto HMAC-SHA256 over the raw body — matches the server).
 */
const VPS_BASE = 'https://hive.campbell.fish';
const ID_KEY = 'hive_module_identity_v1';
const BASE_URL = import.meta.env.BASE_URL;        // '/app/' in prod
const PUSH_INTERVAL_MS = 30000;

declare global {
  interface Window { createHiveModule?: (opts?: Record<string, unknown>) => Promise<any>; }
}

export interface ModuleIdentity { colony_id: string; secret: string; enrolled: boolean; }

function randHex(bytes: number): string {
  const b = new Uint8Array(bytes);
  crypto.getRandomValues(b);
  return [...b].map(x => x.toString(16).padStart(2, '0')).join('');
}
export function getIdentity(): ModuleIdentity {
  try { const raw = localStorage.getItem(ID_KEY); if (raw) return JSON.parse(raw); } catch { /**/ }
  const id: ModuleIdentity = { colony_id: 'app-' + randHex(6), secret: randHex(24), enrolled: false };
  localStorage.setItem(ID_KEY, JSON.stringify(id));
  return id;
}
function saveIdentity(id: ModuleIdentity) { localStorage.setItem(ID_KEY, JSON.stringify(id)); }

async function hmacHex(secret: string, body: string): Promise<string> {
  const enc = new TextEncoder();
  const key = await crypto.subtle.importKey('raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
  const sig = await crypto.subtle.sign('HMAC', key, enc.encode(body));
  return [...new Uint8Array(sig)].map(b => b.toString(16).padStart(2, '0')).join('');
}

let scriptPromise: Promise<void> | null = null;
function loadScript(): Promise<void> {
  if (window.createHiveModule) return Promise.resolve();
  if (scriptPromise) return scriptPromise;
  scriptPromise = new Promise((resolve, reject) => {
    const s = document.createElement('script');
    s.src = `${BASE_URL}hive/hive.js`;
    s.async = true;
    s.onload = () => resolve();
    s.onerror = () => reject(new Error('failed to load hive.js'));
    document.head.appendChild(s);
  });
  return scriptPromise;
}

class LocalModule {
  canvas: HTMLCanvasElement = document.createElement('canvas');
  identity: ModuleIdentity = getIdentity();
  private M: any = null;
  private fns: Record<string, any> = {};
  private started = false;
  private startPromise: Promise<void> | null = null;
  private W = 480; private H = 320;
  private image: ImageData | null = null;
  private lastPush = 0;
  private lastFrame = 0;

  start(): Promise<void> {
    if (this.startPromise) return this.startPromise;
    this.startPromise = (async () => {
      await loadScript();
      const M = await window.createHiveModule!({ locateFile: (p: string) => `${BASE_URL}hive/${p}` });
      this.M = M;
      const cw = (n: string, ret: any, args: any[]) => M.cwrap(n, ret, args);
      this.fns = {
        boot: cw('host_boot', null, ['number']),
        setNow: cw('host_set_now', null, ['number']),
        step: cw('host_step', null, ['number']),
        fb: cw('host_fb', 'number', []),
        w: cw('host_w', 'number', []),
        h: cw('host_h', 'number', []),
        tap: cw('host_tap', null, ['number', 'number']),
        gather: cw('host_gather', null, ['number', 'number']),
        gatherEnd: cw('host_gather_end', null, []),
        selected: cw('host_selected', 'number', []),
        snapshot: cw('host_snapshot', 'number', []),
        snapshotPtr: cw('host_snapshot_ptr', 'number', []),
      };
      this.fns.setNow(Date.now() / 1000);
      this.fns.boot(120);                     // fast-forward ~2h founding
      this.W = this.fns.w(); this.H = this.fns.h();
      this.canvas.width = this.W; this.canvas.height = this.H;
      this.canvas.style.imageRendering = 'pixelated';
      const ctx = this.canvas.getContext('2d', { alpha: false })!;
      this.image = ctx.createImageData(this.W, this.H);
      this.started = true;
      this.lastFrame = performance.now();
      this.loop(ctx);
      this.pushLoop();
    })();
    return this.startPromise;
  }

  private loop(ctx: CanvasRenderingContext2D) {
    const tick = (now: number) => {
      if (!this.started) return;
      const dt = now - this.lastFrame; this.lastFrame = now;
      this.fns.setNow(Date.now() / 1000);
      this.fns.step(dt);
      const p = this.fns.fb();
      const fb = this.M.HEAPU16.subarray(p >> 1, (p >> 1) + this.W * this.H);
      const rgba = this.image!.data;
      for (let i = 0, o = 0; i < this.W * this.H; i++, o += 4) {
        const v = fb[i];
        const r5 = (v >> 11) & 0x1f, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
        rgba[o] = (r5 << 3) | (r5 >> 2);
        rgba[o + 1] = (g6 << 2) | (g6 >> 4);
        rgba[o + 2] = (b5 << 3) | (b5 >> 2);
        rgba[o + 3] = 255;
      }
      ctx.putImageData(this.image!, 0, 0);
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }

  private async pushLoop() {
    // Push immediately (enrols), then every PUSH_INTERVAL_MS.
    while (this.started) {
      try { await this.pushNow(); } catch (e) { console.warn('[module] push failed', e); }
      await new Promise(r => setTimeout(r, PUSH_INTERVAL_MS));
    }
  }

  async pushNow(): Promise<number> {
    const len = this.fns.snapshot();
    const bytes = this.M.HEAPU8.slice(this.fns.snapshotPtr(), this.fns.snapshotPtr() + len);
    let json = new TextDecoder().decode(bytes);
    json = json.replace('"colony_id":""', `"colony_id":"${this.identity.colony_id}"`);
    const sig = await hmacHex(this.identity.secret, json);
    const headers: Record<string, string> = { 'Content-Type': 'application/json', 'x-hmac-sha256': sig };
    if (!this.identity.enrolled) headers['x-enroll-secret'] = this.identity.secret;
    const res = await fetch(`${VPS_BASE}/api/v1/colonies/${this.identity.colony_id}/snapshot`, {
      method: 'POST', headers, body: json,
    });
    if (res.ok && !this.identity.enrolled) { this.identity.enrolled = true; saveIdentity(this.identity); }
    this.lastPush = Date.now();
    return res.status;
  }

  private toPx(clientX: number, clientY: number): [number, number] {
    const r = this.canvas.getBoundingClientRect();
    return [Math.floor((clientX - r.left) / r.width * this.W),
            Math.floor((clientY - r.top) / r.height * this.H)];
  }
  tapAt(clientX: number, clientY: number) { const [x, y] = this.toPx(clientX, clientY); this.fns.tap(x, y); }
  gatherAt(clientX: number, clientY: number) { const [x, y] = this.toPx(clientX, clientY); this.fns.gather(x, y); }
  gatherEnd() { this.fns.gatherEnd(); }
  selected(): number { return this.fns.selected(); }
}

export const localModule = new LocalModule();
