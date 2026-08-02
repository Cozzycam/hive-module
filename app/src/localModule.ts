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

// FNV-1a → uint32. Seeds the sim from the install's unique colony_id so each
// phone founds a DISTINCT colony (and re-founds the same one across reloads,
// until IDBFS persistence lands). Without this every install shared one seed
// and produced the identical queen + cast.
function seedFromString(s: string): number {
  let h = 0x811c9dc5;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 0x01000193);
  }
  return h >>> 0;
}
export function getIdentity(): ModuleIdentity {
  try { const raw = localStorage.getItem(ID_KEY); if (raw) return JSON.parse(raw); } catch { /**/ }
  const id: ModuleIdentity = { colony_id: 'app-' + randHex(6), secret: randHex(24), enrolled: false };
  localStorage.setItem(ID_KEY, JSON.stringify(id));
  return id;
}
function saveIdentity(id: ModuleIdentity) { localStorage.setItem(ID_KEY, JSON.stringify(id)); }

// Flag read by Empty.tsx on the next load to auto-boot the fresh colony.
export const AUTOSTART_KEY = 'hive_autostart_module';
// Set by the "Start a fresh colony" reset; the next boot wipes the persisted
// /colony dir so the module founds from scratch instead of restoring.
export const WIPE_KEY = 'hive_wipe_persistence';
// Wall-clock unix of the last time this install's module was active. On reopen
// the module fast-forwards by (now − this) so the colony lives in real time even
// while the app was closed (like an always-on hardware module).
export const LAST_ACTIVE_KEY = 'hive_last_active';

// Mint a brand-new install identity → a genuinely fresh, unique colony. The
// colony seed is derived from colony_id, so a new id means a new queen + cast.
// The running WASM still holds the OLD colony, so the caller must reload to get
// a clean instance that founds the new one (there's no cross-reload persistence
// yet, so nothing is lost). Returns the new identity.
export function resetColonyIdentity(): ModuleIdentity {
  const id: ModuleIdentity = { colony_id: 'app-' + randHex(6), secret: randHex(24), enrolled: false };
  saveIdentity(id);
  return id;
}

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
  private syncing = false;
  private W = 480; private H = 320;
  private image: ImageData | null = null;
  private lastPush = 0;
  private lastFrame = 0;

  start(onStatus?: (s: string) => void): Promise<void> {
    if (this.startPromise) return this.startPromise;
    const step = (s: string) => { try { onStatus?.(s); } catch { /**/ } };
    this.startPromise = (async () => {
      step('loading engine');
      await loadScript();
      step('starting engine');
      const M = await window.createHiveModule!({ locateFile: (p: string) => `${BASE_URL}hive/${p}` });
      this.M = M;
      step('opening storage');
      // Never let storage setup block boot — race the whole thing against a hard
      // cap so IndexedDB trouble (mobile/incognito) can't wedge startup.
      await Promise.race([
        this.mountPersistence(M),
        new Promise<void>((r) => setTimeout(r, 6000)),
      ]);
      step('building colony');
      const cw = (n: string, ret: any, args: any[]) => M.cwrap(n, ret, args);
      this.fns = {
        boot: cw('host_boot', null, ['number']),
        bootInc: cw('host_boot_incubation', null, []),
        feedPrincess: cw('host_feed_princess', null, []),
        warp: cw('host_warp', null, ['number', 'number']),
        prMat: cw('host_pr_maturity', 'number', []),
        prBond: cw('host_pr_bond', 'number', []),
        prHunger: cw('host_pr_hunger', 'number', []),
        prDormant: cw('host_pr_dormant', 'number', []),
        prHatched: cw('host_pr_hatched', 'number', []),
        prPx: cw('host_pr_px', 'number', []),
        prPy: cw('host_pr_py', 'number', []),
        prBoredom: cw('host_pr_boredom', 'number', []),
        prSocial: cw('host_pr_social', 'number', []),
        prRest: cw('host_pr_rest', 'number', []),
        foundedUnix: cw('host_founded_unix', 'number', []),
        conkers: cw('host_conkers', 'number', []),
        seed: cw('host_seed', null, ['number']),
        catchup: cw('host_catchup', null, ['number']),
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
        exportPrincess: cw('host_export_princess', 'number', []),
        exportPrincessPtr: cw('host_export_princess_ptr', 'number', []),
        eventsPtr: cw('host_events_ptr', 'number', []),
        eventsLen: cw('host_events_len', 'number', []),
        eventsClear: cw('host_events_clear', null, []),
        grantWish: cw('host_grant_wish', null, ['number']),
        feedColony: cw('host_feed_colony', null, ['number']),
        carePackage: cw('host_care_package', null, []),
        rename: cw('host_rename', null, ['number', 'string']),
        setTint: cw('host_set_tint', null, ['number', 'number', 'number']),
        tintConker: cw('host_tint_conker', null, ['number', 'number']),
        setColonyTitle: cw('host_set_colony_title', null, ['string']),
        throwBall: cw('host_throw_ball', null, ['number', 'number']),
        prBall: cw('host_pr_ball', 'number', []),
        buyDecor: cw('host_buy_decor', 'number', ['number']),
        bugs: cw('host_bugs', 'number', []),
        shopJson: cw('host_shop_json', 'string', []),
        owned: cw('host_owned', 'number', []),
        water: cw('host_water', 'number', []),
        flowering: cw('host_flowering', 'number', []),
        interactReady: cw('host_interact_ready', 'number', ['number']),
        unequip: cw('host_unequip', 'number', []),
        worn: cw('host_worn', 'number', []),
        roomX: cw('host_room_x', 'number', []),
        roomY: cw('host_room_y', 'number', []),
        roomW: cw('host_room_w', 'number', []),
        roomH: cw('host_room_h', 'number', []),
      };
      // Unique colony per install — must precede boot. Guarded so a stale cached
      // hive.wasm without host_seed (mid-PWA-update skew) degrades to a default
      // colony instead of throwing and leaving the button dead.
      try { this.fns.seed(seedFromString(this.identity.colony_id)); }
      catch (e) { console.warn('[module] host_seed unavailable (stale wasm?)', e); }
      this.fns.setNow(Date.now() / 1000);
      this.applyClock();
      this.fetchWeather();
      setInterval(() => this.fetchWeather(), 15 * 60 * 1000);
      this.fns.bootInc();                     // tamagotchi: raise ONE princess (no queen/colony) — she becomes a queen only at coronation, on a module

      this.catchUpFromLastActive();           // fast-forward the time the app was closed (always-on-module behaviour)
      this.W = this.fns.w(); this.H = this.fns.h();
      this.canvas.width = this.W; this.canvas.height = this.H;
      this.canvas.style.imageRendering = 'pixelated';
      const ctx = this.canvas.getContext('2d', { alpha: false })!;
      this.image = ctx.createImageData(this.W, this.H);
      this.started = true;
      this.lastFrame = performance.now();
      step('running');
      this.loop(ctx);
      this.pushLoop();
      setInterval(() => this.persist(), 10000);   // flush colony state to IndexedDB
      if (typeof document !== 'undefined') {
        document.addEventListener('visibilitychange', () => {
          if (document.hidden) { this.markActive(); this.persist(); }
          else this.catchUpFromLastActive();   // foreground resume → fast-forward the background gap
        });
        // bfcache restore (mobile) resumes the frozen page without re-running start()
        window.addEventListener('pageshow', (e) => { if ((e as PageTransitionEvent).persisted) this.catchUpFromLastActive(); });
      }
    })();
    return this.startPromise;
  }

  // Mount /colony on IDBFS so the firmware's registry persistence (manifest,
  // identities, brood, bonds) survives reloads. Must run before host_boot: the
  // registry reads /colony/manifest.json at init and restores the colony (Case
  // C) instead of re-founding. If IDBFS is unavailable the module still runs,
  // just ephemerally (per session).
  private async mountPersistence(M: any): Promise<void> {
    try {
      const FS = M.FS;
      if (!FS?.filesystems?.IDBFS) { console.warn('[module] IDBFS unavailable — colony will not persist'); return; }
      try { FS.mkdir('/colony'); } catch { /* already exists */ }
      FS.mount(FS.filesystems.IDBFS, {}, '/colony');
      // Load persisted colony, but NEVER let a slow/stuck IndexedDB block boot
      // (iOS home-screen PWAs can stall syncfs). Time out → run from whatever
      // loaded; the colony simply won't restore this once.
      await this.syncfsSafe(FS, true);
      if (localStorage.getItem(WIPE_KEY)) {                             // reset flow → start fresh
        this.wipeColonyDir(FS);
        await this.syncfsSafe(FS, false);
        localStorage.removeItem(WIPE_KEY);
      }
    } catch (e) {
      console.warn('[module] persistence mount failed — running ephemerally', e);
    }
  }

  // syncfs that always resolves — on callback, on error, or after a timeout —
  // so IndexedDB flakiness can never wedge startup.
  private syncfsSafe(FS: any, populate: boolean, ms = 4000): Promise<void> {
    return new Promise<void>((resolve) => {
      let done = false;
      const finish = () => { if (!done) { done = true; resolve(); } };
      const timer = setTimeout(finish, ms);
      try { FS.syncfs(populate, () => { clearTimeout(timer); finish(); }); }
      catch { clearTimeout(timer); finish(); }
    });
  }

  // Empty /colony (keep the mount point) — used by the reset flow so a fresh
  // identity founds a clean colony instead of restoring the old one.
  private wipeColonyDir(FS: any) {
    const rm = (dir: string) => {
      let names: string[] = [];
      try { names = FS.readdir(dir).filter((n: string) => n !== '.' && n !== '..'); } catch { return; }
      for (const n of names) {
        const p = `${dir}/${n}`;
        try {
          if (FS.isDir(FS.stat(p).mode)) { rm(p); FS.rmdir(p); }
          else FS.unlink(p);
        } catch { /* best effort */ }
      }
    };
    rm('/colony');
  }

  // Record "the module was active now" — read on the next open to size catch-up.
  private markActive(): void {
    try { localStorage.setItem(LAST_ACTIVE_KEY, String(Math.floor(Date.now() / 1000))); } catch { /**/ }
  }

  // Fast-forward the colony by however long it was inactive, then re-mark now.
  // Called on boot AND every time the app returns to the foreground — on mobile
  // you background/resume far more often than you reload, and the sim clock only
  // advances while foregrounded, so without this the eggs' timers stall.
  private catchUpFromLastActive(): void {
    if (!this.fns?.catchup) return;
    try {
      const last = Number(localStorage.getItem(LAST_ACTIVE_KEY) || 0);
      if (last > 0) {
        const away = Math.floor(Date.now() / 1000) - last;
        if (away > 60) this.fns.catchup(away);
      }
    } catch (e) { console.warn('[module] catch-up skipped', e); }
    this.markActive();
  }

  // Flush the in-memory FS to IndexedDB (only dirty files are written). Never
  // wedges: the syncing flag self-clears via timeout if the callback stalls.
  private persist(): void {
    if (this.syncing || !this.M?.FS) return;
    this.syncing = true;
    const done = () => { this.syncing = false; };
    const timer = setTimeout(done, 5000);
    try { this.M.FS.syncfs(false, () => { clearTimeout(timer); done(); }); }
    catch { clearTimeout(timer); done(); }
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
    // Dev: run the module fully in-process — don't enrol/push to the live VPS
    // (the tamagotchi reads its princess straight from here; the push is only for
    // backup/cross-device, which local dev doesn't need). Prod pushes normally.
    if (import.meta.env.DEV) return;
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
      case 'tint_conker':      this.tintConker(Number(payload?.id ?? 0), Number(payload?.tint ?? 0)); return true;
      case 'buy_decor':        this.buyDecor(Number(payload?.item ?? 0)); return true;
      case 'set_colony_title': try { this.fns.setColonyTitle?.(String(payload?.title ?? '')); } catch { /* stale wasm */ } return true;
      case 'set_floor_tint': {
        const rgb = payload?.rgb ?? 0;
        this.fns.setTint((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff); return true;
      }
      case 'set_followed':     return true;   // pins are app-side; on-glass stars are cosmetic
      default: return false;
    }
  }

  async pushNow(): Promise<number> {
    if (import.meta.env.DEV) return 200;   // dev: run in-process, don't enrol on the live VPS
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
    this.markActive();   // keep last-active fresh so catch-up measures from here
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

  // ---- Incubation / princess (tamagotchi) surface for the Her care screen ----
  // The screen renders a zoomed portrait crop of the live framebuffer centred on
  // her; these read the same in-process WASM the loop ticks (no VPS round-trip).
  isRunning(): boolean { return this.started; }
  frameImage(): ImageData | null { return this.image; }        // full 480x320 RGBA, updated each loop tick
  dims() { return { w: this.W, h: this.H }; }
  tapFb(fx: number, fy: number) { try { this.fns.tap(Math.round(fx), Math.round(fy)); } catch { /**/ } }
  feedPrincess() { try { this.fns.feedPrincess(); } catch { /**/ } }
  conkerCount(): number { try { return this.fns.conkers() ?? 0; } catch { return 0; } }
  // The full colony snapshot (the same JSON pushed to the VPS) read in-process —
  // lets the About screen show her name/personality/traits without a VPS round-trip.
  colonySnapshot(): Record<string, unknown> | null {
    if (!this.started || !this.fns.snapshot) return null;
    try {
      const len = this.fns.snapshot();
      const bytes = this.M.HEAPU8.slice(this.fns.snapshotPtr(), this.fns.snapshotPtr() + len);
      return JSON.parse(new TextDecoder().decode(bytes));
    } catch { return null; }
  }
  renameConker(id: number, name: string) { try { this.fns.rename(id >>> 0, name); } catch { /**/ } }
  // Gateway coronation: serialize the raised princess (name, colour,
  // personality, traits, bond — her whole identity) for the VPS handoff.
  exportPrincess(): Record<string, unknown> | null {
    if (!this.started || !this.fns.exportPrincess) return null;
    try {
      const len = this.fns.exportPrincess();
      if (!len) return null;
      const bytes = this.M.HEAPU8.slice(this.fns.exportPrincessPtr(), this.fns.exportPrincessPtr() + len);
      return JSON.parse(new TextDecoder().decode(bytes));
    } catch { return null; }
  }
  // Park her on the VPS under a one-time claim token (HMAC-signed by this
  // install's colony — the same auth as the snapshot push). Returns the token
  // the module's summon_queen command will claim, or null on failure.
  async parkHandoff(): Promise<string | null> {
    const p = this.exportPrincess();
    if (!p) return null;
    p.from_colony = this.identity.colony_id;   // in-process manifest id is blank; stamp like pushNow
    const body = JSON.stringify(p);
    const sig = await hmacHex(this.identity.secret, body);
    const headers: Record<string, string> = { 'Content-Type': 'application/json', 'x-hmac-sha256': sig };
    if (!this.identity.enrolled) headers['x-enroll-secret'] = this.identity.secret;
    try {
      const res = await fetch(`${VPS_BASE}/api/v1/colonies/${this.identity.colony_id}/handoff`, {
        method: 'POST', headers, body,
      });
      if (!res.ok) return null;
      const j = await res.json();
      return typeof j.token === 'string' ? j.token : null;
    } catch { return null; }
  }
  // Dev-only: fast-forward sim time (fed = an attentive keeper feeds; else neglect).
  warp(seconds: number, fed: boolean) { try { this.fns.warp(seconds, fed ? 1 : 0); } catch { /**/ } }
  princessStats(): PrincessStats | null {
    if (!this.started || !this.fns.prHatched) return null;
    try {
      return {
        maturity: (this.fns.prMat() ?? 0) / 1000,     // 0..1
        bond:     this.fns.prBond() ?? 0,             // 0..100
        hunger:   this.fns.prHunger() ?? 0,
        dormant:  !!this.fns.prDormant(),
        hatched:  !!this.fns.prHatched(),
        boredom:  this.fns.prBoredom() ?? 0,
        social:   this.fns.prSocial() ?? 0,
        rest:     this.fns.prRest() ?? 0,
        founded:  this.fns.foundedUnix() ?? 0,
        px:       this.fns.prPx() ?? (this.W / 2),
        py:       this.fns.prPy() ?? (this.H / 2),
        // Guarded: an install running a cached pre-v220 wasm has no host_pr_ball,
        // and a dead throw button beats a crashed Nest.
        ball:     (this.fns.prBall ? this.fns.prBall() : 0) ?? 0,
      };
    } catch { return null; }
  }

  // Water her — the bud on her head opens into a flower for a few hours.
  water(): boolean {
    if (!this.started || !this.fns.water) return false;
    try { return this.fns.water() === 1; } catch { return false; }
  }

  // Seconds of flowering left (0 = not flowering).
  flowering(): number {
    if (!this.started || !this.fns.flowering) return 0;
    try { return this.fns.flowering() ?? 0; } catch { return 0; }
  }

  // Seconds until this interaction pays full company again (0 = ready).
  // The interaction always WORKS — this is only about the payoff.
  interactReady(kind: number): number {
    if (!this.started || !this.fns.interactReady) return 0;
    try { return this.fns.interactReady(kind) ?? 0; } catch { return 0; }
  }

  // Throw the ball at a spot the keeper picked, rather than one we chose.
  throwBallAt(fx: number, fy: number) {
    if (!this.started || !this.fns.throwBall) return;
    try { this.fns.throwBall(Math.round(fx), Math.round(fy)); } catch { /**/ }
  }

  // Throw the ball a short hop from her — far enough to be a chase, close enough
  // to STAY ON SCREEN. The Nest shows only a 174x232 window of the 480x320
  // chamber, following her; throwing "as far from her as possible" (the first
  // version) put the ball outside that window essentially every time, so the
  // keeper saw no ball and only a vague wander. Anything within ~80px of her is
  // inside the window on every phone.
  throwBall() {
    if (!this.started || !this.fns.throwBall) return;
    try {
      const px = this.fns.prPx() ?? this.W / 2;
      const py = this.fns.prPy() ?? this.H / 2;
      const margin = 24;
      const clamp = (v: number, hi: number) => (v < margin ? margin : v > hi - margin ? hi - margin : v);
      // A random direction at a readable distance; retry a couple of times so a
      // throw near a wall still lands somewhere she has to travel to.
      let bx = px, by = py;
      for (let i = 0; i < 6; i++) {
        const ang = Math.random() * Math.PI * 2;
        const dist = 50 + Math.random() * 30;          // ~3-5 cells
        bx = clamp(px + Math.cos(ang) * dist, this.W);
        by = clamp(py + Math.sin(ang) * dist, this.H);
        if ((bx - px) ** 2 + (by - py) ** 2 > 30 * 30) break;
      }
      this.fns.throwBall(Math.round(bx), Math.round(by));
    } catch { /* stale wasm */ }
  }

  tintConker(id: number, tint: number) {
    try { this.fns.tintConker?.(id >>> 0, tint & 0xff); } catch { /**/ }
  }

  // ---- the decor shop ----
  // Catalogue comes from the SIM, never a copy here: a price that drifted
  // between the two would only show up as a keeper being charged the wrong
  // amount. Returns [] on a wasm too old to have a shop.
  shopItems(): { id: number; name: string; price: number; tint: number; wear: number }[] {
    if (!this.started || !this.fns.shopJson) return [];
    try { return JSON.parse(this.fns.shopJson() || '[]'); } catch { return []; }
  }

  bugs(): number {
    if (!this.started || !this.fns.bugs) return 0;
    try { return this.fns.bugs() ?? 0; } catch { return 0; }
  }

  // Bitmask of items already paid for — owned things can be swapped to for free.
  ownedItems(): number {
    if (!this.started || !this.fns.owned) return 0;
    try { return this.fns.owned() ?? 0; } catch { return 0; }
  }

  // What she's carrying (0 = nothing).
  wornItem(): number {
    if (!this.started || !this.fns.worn) return 0;
    try { return this.fns.worn() ?? 0; } catch { return 0; }
  }

  unequip(): boolean {
    if (!this.started || !this.fns.unequip) return false;
    try { return this.fns.unequip() === 1; } catch { return false; }
  }

  // The room's floor colour (feedback #59). cmd_set_floor_tint already existed
  // for physical modules — this just points it at her room.
  setRoomTint(r: number, g: number, b: number) {
    try { this.fns.setTint?.(r | 0, g | 0, b | 0); } catch { /**/ }
  }

  buyDecor(item: number): boolean {
    if (!this.started || !this.fns.buyDecor) return false;
    try { return this.fns.buyDecor(item | 0) === 1; } catch { return false; }
  }

  // Her room, in framebuffer pixels — the Nest crops exactly this, so there is no
  // camera. Returns null on an install still running a cached pre-v223 wasm with
  // no room exports, so the Nest can fall back to the old follow-cam.
  room(): { x: number; y: number; w: number; h: number } | null {
    if (!this.started || !this.fns.roomW) return null;
    try {
      const w = this.fns.roomW(), h = this.fns.roomH();
      if (!w || !h) return null;
      return { x: this.fns.roomX(), y: this.fns.roomY(), w, h };
    } catch { return null; }
  }
}

export interface PrincessStats {
  maturity: number; bond: number; hunger: number; dormant: boolean; hatched: boolean;
  boredom: number; social: number; rest: number; founded: number; px: number; py: number;
  ball: number;
}

export const localModule = new LocalModule();
if (import.meta.env.DEV) (globalThis as { localModule?: LocalModule }).localModule = localModule;
