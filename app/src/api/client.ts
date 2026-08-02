import type {
  ColonySnapshot,
  ColoniesListResponse,
  ColonyEvent,
  EventsResponse,
  HealthResponse,
  LilGuyListResponse,
  LilGuyDetail,
  DataSource,
} from './types';

const VPS_BASE = 'https://hive.campbell.fish';
const LAN_TIMEOUT = 1000;
const VPS_TIMEOUT = 8000;

const CACHE_KEY_SNAPSHOT = 'hive_snapshot';
const CACHE_KEY_TIMESTAMP = 'hive_snapshot_ts';
const CACHE_KEY_EVENTS = 'hive_events';
const CACHE_KEY_LAN_IP = 'hive_lan_ip';
const CACHE_KEY_COLONY_ID = 'hive_colony_id';

// ---- localStorage helpers ----

export function getStoredColonyId(): string | null {
  return localStorage.getItem(CACHE_KEY_COLONY_ID);
}

export function setStoredColonyId(id: string): void {
  localStorage.setItem(CACHE_KEY_COLONY_ID, id);
}

export function getStoredLanIp(): string | null {
  return localStorage.getItem(CACHE_KEY_LAN_IP);
}

export function setStoredLanIp(ip: string): void {
  localStorage.setItem(CACHE_KEY_LAN_IP, ip);
}

export function clearConnection(): void {
  // The Annals register is per-colony — read the id before it goes
  const colonyId = localStorage.getItem(CACHE_KEY_COLONY_ID);
  if (colonyId) localStorage.removeItem(`hive_annals_${colonyId}`);
  localStorage.removeItem('hive_last_deed_seen_unix');
  localStorage.removeItem('hive_observance_seen');
  localStorage.removeItem(CACHE_KEY_COLONY_ID);
  localStorage.removeItem(CACHE_KEY_LAN_IP);
  localStorage.removeItem(CACHE_KEY_SNAPSHOT);
  localStorage.removeItem(CACHE_KEY_TIMESTAMP);
  localStorage.removeItem(CACHE_KEY_EVENTS);
}

function cacheSnapshot(snapshot: ColonySnapshot): void {
  try {
    localStorage.setItem(CACHE_KEY_SNAPSHOT, JSON.stringify(snapshot));
    localStorage.setItem(CACHE_KEY_TIMESTAMP, String(Date.now()));
  } catch { /* localStorage full — non-fatal */ }
}

function getCachedSnapshot(): { snapshot: ColonySnapshot; timestamp: number } | null {
  const raw = localStorage.getItem(CACHE_KEY_SNAPSHOT);
  const ts = localStorage.getItem(CACHE_KEY_TIMESTAMP);
  if (!raw || !ts) return null;
  try {
    return { snapshot: JSON.parse(raw), timestamp: Number(ts) };
  } catch { return null; }
}

function cacheEvents(events: EventsResponse): void {
  try {
    localStorage.setItem(CACHE_KEY_EVENTS, JSON.stringify(events));
  } catch { /* non-fatal */ }
}

function getCachedEvents(): EventsResponse | null {
  const raw = localStorage.getItem(CACHE_KEY_EVENTS);
  if (!raw) return null;
  try { return JSON.parse(raw); } catch { return null; }
}

// ---- Fetch helpers ----

async function fetchWithTimeout(url: string, timeoutMs: number): Promise<Response> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const res = await fetch(url, { signal: controller.signal });
    return res;
  } finally {
    clearTimeout(timer);
  }
}

async function tryFetchJson<T>(url: string, timeoutMs: number): Promise<T | null> {
  try {
    const res = await fetchWithTimeout(url, timeoutMs);
    if (!res.ok) return null;
    return await res.json() as T;
  } catch {
    return null;
  }
}

// ---- Public API ----

export interface FetchResult<T> {
  data: T;
  source: DataSource;
  timestamp: number;
}

export async function fetchSnapshot(
  colonyId: string,
): Promise<FetchResult<ColonySnapshot> | null> {
  const lanIp = getStoredLanIp();

  // 1. Try LAN queen
  if (lanIp) {
    const data = await tryFetchJson<ColonySnapshot>(
      `http://${lanIp}/api/v1/colony`,
      LAN_TIMEOUT,
    );
    if (data) {
      cacheSnapshot(data);
      return { data, source: 'lan', timestamp: Date.now() };
    }
  }

  // 2. Try VPS
  const vpsData = await tryFetchJson<ColonySnapshot>(
    `${VPS_BASE}/api/v1/colonies/${colonyId}`,
    VPS_TIMEOUT,
  );
  if (vpsData) {
    cacheSnapshot(vpsData);
    return { data: vpsData, source: 'vps', timestamp: Date.now() };
  }

  // 3. Fall back to cache
  const cached = getCachedSnapshot();
  if (cached) {
    return { data: cached.snapshot, source: 'cache', timestamp: cached.timestamp };
  }

  return null;
}

export async function fetchEvents(
  colonyId: string,
  since = 0,
  limit = 200,
  type?: string,   // optional comma-separated type filter (e.g. 'crafted,art_weathered')
): Promise<FetchResult<EventsResponse> | null> {
  const lanIp = getStoredLanIp();
  const q = `since=${since}&limit=${limit}${type ? `&type=${encodeURIComponent(type)}` : ''}`;

  if (lanIp) {
    const data = await tryFetchJson<EventsResponse>(
      `http://${lanIp}/api/v1/colony/events?${q}`,
      LAN_TIMEOUT,
    );
    if (data) {
      if (!type) cacheEvents(data);   // don't let a filtered fetch clobber the general cache
      return { data, source: 'lan', timestamp: Date.now() };
    }
  }

  const vpsData = await tryFetchJson<EventsResponse>(
    `${VPS_BASE}/api/v1/colonies/${colonyId}/events?${q}`,
    VPS_TIMEOUT,
  );
  if (vpsData) {
    if (!type) cacheEvents(vpsData);
    return { data: vpsData, source: 'vps', timestamp: Date.now() };
  }

  const cached = getCachedEvents();
  if (cached) {
    return { data: cached, source: 'cache', timestamp: Date.now() };
  }

  return null;
}

export async function fetchLilguyEvents(
  colonyId: string,
  lilguyId: number,
  since = 0,
  limit = 200,
): Promise<EventsResponse | null> {
  const lanIp = getStoredLanIp();

  if (lanIp) {
    const data = await tryFetchJson<EventsResponse>(
      `http://${lanIp}/api/v1/colony/lilguys/${lilguyId}/events?since=${since}&limit=${limit}`,
      LAN_TIMEOUT,
    );
    if (data) return data;
  }

  return tryFetchJson<EventsResponse>(
    `${VPS_BASE}/api/v1/colonies/${colonyId}/lilguys/${lilguyId}/events?since=${since}&limit=${limit}`,
    VPS_TIMEOUT,
  );
}

// Per-conker history, shared by the roster list AND the profile so both
// compute epithets from the same events (keeper #37). The colony events feed
// cannot serve here: it is a rolling newest-200 window colony-wide, so a
// lifetime title ('the Maker') earned before the window has scrolled out of
// it while this per-conker endpoint still returns it — the two views would
// disagree persistently, not just on deep histories. Cached with in-flight
// dedup so the list (one call per conker) doesn't refetch on every poll.
const LILGUY_HISTORY_TTL_MS = 5 * 60_000;
const lilguyHistoryCache = new Map<string, { at: number; promise: Promise<ColonyEvent[] | null> }>();

export function fetchLilguyHistory(
  colonyId: string,
  lilguyId: number,
): Promise<ColonyEvent[] | null> {
  const key = `${colonyId}:${lilguyId}`;
  const hit = lilguyHistoryCache.get(key);
  if (hit && Date.now() - hit.at < LILGUY_HISTORY_TTL_MS) return hit.promise;
  const promise: Promise<ColonyEvent[] | null> =
    fetchLilguyEvents(colonyId, lilguyId).then(r => {
      if (!r) {
        // Failed fetch — drop it so the next caller retries instead of
        // pinning a null for the whole TTL
        if (lilguyHistoryCache.get(key)?.promise === promise) {
          lilguyHistoryCache.delete(key);
        }
        return null;
      }
      return r.results;
    });
  lilguyHistoryCache.set(key, { at: Date.now(), promise });
  return promise;
}

export async function fetchLilguys(
): Promise<LilGuyListResponse | null> {
  // Only available from queen LAN
  const lanIp = getStoredLanIp();
  if (!lanIp) return null;
  return tryFetchJson<LilGuyListResponse>(
    `http://${lanIp}/api/v1/lilguys`,
    LAN_TIMEOUT,
  );
}

export async function fetchLilguyDetail(
  lilguyId: number,
): Promise<LilGuyDetail | null> {
  // Only available from queen LAN
  const lanIp = getStoredLanIp();
  if (!lanIp) return null;
  return tryFetchJson<LilGuyDetail>(
    `http://${lanIp}/api/v1/lilguys/${lilguyId}`,
    LAN_TIMEOUT,
  );
}

export async function fetchColonies(): Promise<ColoniesListResponse | null> {
  return tryFetchJson<ColoniesListResponse>(
    `${VPS_BASE}/api/v1/colonies`,
    VPS_TIMEOUT,
  );
}

// Queue a command for the queen (applied on her next VPS poll, ~30s)
export async function sendCommand(
  colonyId: string,
  type: 'name_conker' | 'feed_colony' | 'set_module_role' | 'set_floor_tint' | 'tint_conker' | 'set_colony_title' | 'gift_care_package' | 'ota_update' | 'set_followed' | 'grant_wish' | 'summon_queen',
  payload: Record<string, unknown>,
): Promise<boolean> {
  // A colony running ON this phone applies commands directly — no VPS round-trip
  // (the local module would never poll the command queue).
  const { localModule, getIdentity } = await import('../localModule');
  if (colonyId === getIdentity().colony_id) {
    return localModule.applyCommand(type, payload);
  }
  try {
    const res = await fetchWithTimeout2(
      `${VPS_BASE}/api/v1/colonies/${colonyId}/commands`,
      { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ type, payload }) },
      VPS_TIMEOUT,
    );
    return res.ok;
  } catch {
    return false;
  }
}

// ---- Web push notifications ----

function urlBase64ToUint8Array(base64: string): Uint8Array {
  const padding = '='.repeat((4 - (base64.length % 4)) % 4);
  const b64 = (base64 + padding).replace(/-/g, '+').replace(/_/g, '/');
  const raw = atob(b64);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++) out[i] = raw.charCodeAt(i);
  return out;
}

export type PushResult = 'ok' | 'denied' | 'unsupported' | 'error';

// Whether this browser can do web push at all (iOS needs an installed PWA)
export function pushSupported(): boolean {
  return 'serviceWorker' in navigator && 'PushManager' in window && 'Notification' in window;
}

export async function pushPermission(): Promise<NotificationPermission | 'unsupported'> {
  if (!pushSupported()) return 'unsupported';
  return Notification.permission;
}

// Request permission, subscribe, and register the subscription against a
// colony. pref decides what the server sends: 'milestones' = headline
// moments + the evening digest; 'all' adds ambient finds.
export async function enablePushNotifications(
  colonyId: string,
  pref: 'milestones' | 'all' = 'all',
): Promise<PushResult> {
  if (!pushSupported()) return 'unsupported';
  try {
    const perm = await Notification.requestPermission();
    if (perm !== 'granted') return 'denied';

    const vapidRes = await fetch(`${VPS_BASE}/api/v1/push/vapid`);
    if (!vapidRes.ok) return 'error';
    const { publicKey } = await vapidRes.json();
    if (!publicKey) return 'error';

    const reg = await navigator.serviceWorker.ready;
    const sub = await reg.pushManager.subscribe({
      userVisibleOnly: true,
      applicationServerKey: urlBase64ToUint8Array(publicKey) as BufferSource,
    });

    const res = await fetch(`${VPS_BASE}/api/v1/colonies/${colonyId}/push/subscribe`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ sub, pref }),
    });
    return res.ok ? 'ok' : 'error';
  } catch {
    return 'error';
  }
}

// Ask the server to fire a test push at this colony's subscriptions.
// Returns how many subscriptions the server knows about (null on failure) —
// lets Settings show "registered" truthfully.
export async function sendTestPush(colonyId: string): Promise<number | null> {
  try {
    const res = await fetch(`${VPS_BASE}/api/v1/colonies/${colonyId}/push/test`, { method: 'POST' });
    if (!res.ok) return null;
    const data = await res.json();
    return typeof data.subscriptions === 'number' ? data.subscriptions : null;
  } catch {
    return null;
  }
}

// Unsubscribe this browser and tell the server to forget it (pref = Off)
export async function disablePushNotifications(colonyId: string): Promise<PushResult> {
  if (!pushSupported()) return 'unsupported';
  try {
    const reg = await navigator.serviceWorker.ready;
    const sub = await reg.pushManager.getSubscription();
    if (!sub) return 'ok';  // nothing to disable
    const endpoint = sub.endpoint;
    await sub.unsubscribe();
    await fetch(`${VPS_BASE}/api/v1/colonies/${colonyId}/push/unsubscribe`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ endpoint }),
    });
    return 'ok';
  } catch {
    return 'error';
  }
}

// Latest published module firmware version (for the "update available" check)
export async function fetchLatestFirmware(): Promise<number | null> {
  try {
    const res = await fetchWithTimeout2(
      `${VPS_BASE}/api/v1/firmware/latest`, {}, VPS_TIMEOUT,
    );
    if (!res.ok) return null;
    const data = await res.json();
    return typeof data.version === 'number' ? data.version : null;
  } catch {
    return null;
  }
}

async function fetchWithTimeout2(url: string, init: RequestInit, timeoutMs: number): Promise<Response> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    return await fetch(url, { ...init, signal: controller.signal });
  } finally {
    clearTimeout(timer);
  }
}

// Send user feedback to the VPS inbox (relayed to the developers)
export async function sendFeedback(
  colonyId: string | null,
  text: string,
  context: Record<string, unknown>,
): Promise<boolean> {
  try {
    const res = await fetchWithTimeout2(
      `${VPS_BASE}/api/v1/feedback`,
      {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ colony_id: colonyId || '', text, context }),
      },
      VPS_TIMEOUT,
    );
    return res.ok;
  } catch {
    return false;
  }
}

export async function fetchHealth(): Promise<HealthResponse | null> {
  return tryFetchJson<HealthResponse>(
    `${VPS_BASE}/api/v1/health`,
    VPS_TIMEOUT,
  );
}

export async function testLanConnection(ip: string): Promise<boolean> {
  const data = await tryFetchJson<{ schema: number }>(
    `http://${ip}/api/v1/health`,
    LAN_TIMEOUT,
  );
  return data !== null;
}
