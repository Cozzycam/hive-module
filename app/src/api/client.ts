import type {
  ColonySnapshot,
  ColoniesListResponse,
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
): Promise<FetchResult<EventsResponse> | null> {
  const lanIp = getStoredLanIp();

  if (lanIp) {
    const data = await tryFetchJson<EventsResponse>(
      `http://${lanIp}/api/v1/colony/events?since=${since}&limit=${limit}`,
      LAN_TIMEOUT,
    );
    if (data) {
      cacheEvents(data);
      return { data, source: 'lan', timestamp: Date.now() };
    }
  }

  const vpsData = await tryFetchJson<EventsResponse>(
    `${VPS_BASE}/api/v1/colonies/${colonyId}/events?since=${since}&limit=${limit}`,
    VPS_TIMEOUT,
  );
  if (vpsData) {
    cacheEvents(vpsData);
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
  type: 'name_conker' | 'feed_colony',
  payload: Record<string, unknown>,
): Promise<boolean> {
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

async function fetchWithTimeout2(url: string, init: RequestInit, timeoutMs: number): Promise<Response> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    return await fetch(url, { ...init, signal: controller.signal });
  } finally {
    clearTimeout(timer);
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
