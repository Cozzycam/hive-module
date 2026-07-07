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

// Open-Meteo WMO weather code -> firmware WeatherCondition
// (0 clear, 1 partly, 2 overcast, 3 fog, 4 drizzle, 5 rain, 6 heavy rain, 7 snow, 8 storm).
function wmoToCondition(code: number): number {
  if (code === 0) return 0;
  if (code === 1 || code === 2) return 1;
  if (code === 3) return 2;
  if (code === 45 || code === 48) return 3;
  if (code >= 51 && code <= 57) return 4;
  if (code === 65 || code === 82) return 6;
  if ((code >= 61 && code <= 64) || code === 80 || code === 81) return 5;
  if ((code >= 71 && code <= 77) || code === 85 || code === 86) return 7;
  if (code >= 95) return 8;
  return 1;
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
        setClock: cw('host_set_clock', null, ['number', 'number', 'number', 'number', 'number']),
        setWeather: cw('host_set_weather', null, ['number', 'number']),
        tap: cw('host_tap', null, ['number', 'number']),
        gather: cw('host_gather', null, ['number', 'number']),
        gatherEnd: cw('host_gather_end', null, []),
        selected: cw('host_selected', 'number', []),
        snapshot: cw('host_snapshot', 'number', []),
        snapshotPtr: cw('host_snapshot_ptr', 'number', []),
        eventsPtr: cw('host_events_ptr', 'number', []),
        eventsLen: cw('host_events_len', 'number', []),
        eventsClear: cw('host_events_clear', null, []),
        grantWish: cw('host_grant_wish', null, ['number']),
        feedColony: cw('host_feed_colony', null, ['number']),
        carePackage: cw('host_care_package', null, []),
        rename: cw('host_rename', null, ['number', 'string']),
        setTint: cw('host_set_tint', null, ['number', 'number', 'number']),
      };
      this.fns.setNow(Date.now() / 1000);
      this.applyClock();
      this.fetchWeather();
      setInterval(() => this.fetchWeather(), 15 * 60 * 1000);
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
      this.applyClock();
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

  // Feed the browser's real local time + day/night into the sim (HUD clock,
  // day-counter, season, scene palette).
  private applyClock() {
    const d = new Date();
    const h = d.getHours() + d.getMinutes() / 60;
    let nf: number, phase: number;   // phase: NIGHT 0, DAWN 1, DAY 2, DUSK 3
    if (h >= 7 && h < 19) { nf = 0; phase = 2; }
    else if (h >= 5 && h < 7) { nf = 1 - (h - 5) / 2; phase = 1; }
    else if (h >= 19 && h < 21) { nf = (h - 19) / 2; phase = 3; }
    else { nf = 1; phase = 0; }
    const startOfYear = new Date(d.getFullYear(), 0, 0);
    const doy = Math.floor((d.getTime() - startOfYear.getTime()) / 86400000);
    this.fns.setClock(d.getHours(), d.getMinutes(), doy, nf, phase);
  }

  // Real local weather (Open-Meteo, CORS-enabled). Uses geolocation only if
  // already granted (no prompt); otherwise a sensible default.
  private async fetchWeather() {
    try {
      let lat = 51.5, lon = -0.13;   // default: London
      try {
        const perm = await navigator.permissions?.query({ name: 'geolocation' as PermissionName });
        if (perm?.state === 'granted') {
          [lat, lon] = await new Promise((res) => navigator.geolocation.getCurrentPosition(
            p => res([p.coords.latitude, p.coords.longitude]), () => res([lat, lon]), { timeout: 5000 }));
        }
      } catch { /* keep default */ }
      const r = await fetch(`https://api.open-meteo.com/v1/forecast?latitude=${lat}&longitude=${lon}&current=weather_code,temperature_2m`);
      const j = await r.json();
      const code = j?.current?.weather_code ?? 0;
      const temp = j?.current?.temperature_2m ?? 15;
      this.fns.setWeather(wmoToCondition(code), temp);
    } catch (e) { /* leave weather as-is */ }
  }

  private async pushLoop() {
    // Push immediately (enrols), then every PUSH_INTERVAL_MS.
    while (this.started) {
      try { await this.pushNow(); await this.pushEvents(); }
      catch (e) { console.warn('[module] push failed', e); }
      await new Promise(r => setTimeout(r, PUSH_INTERVAL_MS));
    }
  }

  private async pushEvents() {
    const len = this.fns.eventsLen();
    if (!len) return;
    const bytes = this.M.HEAPU8.slice(this.fns.eventsPtr(), this.fns.eventsPtr() + len);
    const inner = new TextDecoder().decode(bytes);
    const body = `{"events":[${inner}]}`;
    const sig = await hmacHex(this.identity.secret, body);
    const res = await fetch(`${VPS_BASE}/api/v1/colonies/${this.identity.colony_id}/events`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'x-hmac-sha256': sig },
      body,
    });
    if (res.ok) this.fns.eventsClear();
  }

  // Apply a keeper command straight to the on-phone colony (the app routes
  // here instead of the VPS command queue for a local colony).
  applyCommand(type: string, payload: any): boolean {
    switch (type) {
      case 'grant_wish':       this.fns.grantWish(payload?.id >>> 0); return true;
      case 'feed_colony':      this.fns.feedColony(payload?.amount ?? 25); return true;
      case 'gift_care_package': this.fns.carePackage(); return true;
      case 'name_conker':      this.fns.rename(payload?.id >>> 0, String(payload?.name ?? '')); return true;
      case 'set_floor_tint': {
        const rgb = payload?.rgb ?? 0;
        this.fns.setTint((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); return true;
      }
      case 'set_followed':     return true;   // pins are app-side; on-glass stars are cosmetic
      default: return false;
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
