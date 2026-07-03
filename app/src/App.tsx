import { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import { registerSW } from 'virtual:pwa-register';
import { ColonyContext, ColonyActionsContext, type ColonyState, type ColonyActions } from './state/colony';
import { PinsProvider, usePins } from './state/pins';
import { Poller } from './api/poller';
import { getStoredColonyId, setStoredColonyId, fetchEvents, clearConnection, fetchLatestFirmware, sendCommand, enablePushNotifications } from './api/client';
import { Home } from './screens/Home';
import { Characters } from './screens/Characters';
import { Chambers } from './screens/Chambers';
import { Diary } from './screens/Diary';
import { Feedback } from './screens/Feedback';
import { Empty } from './screens/Empty';
import { Settings, notificationManager } from './screens/Settings';
import { critterEmoji } from './data/critters';
import { FieldGuide } from './screens/FieldGuide';
import { Gallery } from './screens/Gallery';
import { HIVE } from './theme/palette';
import { SIZES } from './theme/fonts';
import type { ColonyEvent, DataSource } from './api/types';

type Tab = 'home' | 'characters' | 'chambers' | 'diary' | 'feedback';

const TAB_LABELS: Record<Tab, string> = {
  home: 'Home',
  characters: 'Characters',
  chambers: 'Chambers',
  diary: 'Diary',
  feedback: 'Feedback',
};

const TAB_ICONS: Record<Tab, string> = {
  home: '\u{1F3E0}',
  characters: '\u{1F330}',
  chambers: '\u{1F9E9}',
  diary: '\u{1F4D6}',
  feedback: '\u{1F4AC}',
};

// localStorage key: unix of the most recent neighbour gift the user has seen.
const GIFT_SEEN_KEY = 'hive_last_gift_seen_unix';
// localStorage key: unix of the most recent critter discovery the user has seen.
const DISCOVERY_SEEN_KEY = 'hive_last_discovery_seen_unix';

// Service worker registration. registerType is 'autoUpdate' (vite.config.ts), so
// a new build activates and reloads on its own — no user-facing update prompt.
// We still poll every minute so a long-open session picks up fresh versions
// without waiting for a relaunch.
registerSW({
  onRegisteredSW(_url, registration) {
    if (registration) {
      setInterval(() => { registration.update().catch(() => {}); }, 60_000);
    }
  },
});

// Auth stub — always passes
function AuthGate({ children }: { children: React.ReactNode }) {
  return <>{children}</>;
}

// Auth hook stub
function useAuth() {
  return { authenticated: true, user: null };
}

export function App() {
  const [colonyId, setColonyIdState] = useState<string | null>(getStoredColonyId);
  const [tab, setTab] = useState<Tab>('home');
  const [showSettings, setShowSettings] = useState(false);
  const [showFieldGuide, setShowFieldGuide] = useState(false);
  const [showGallery, setShowGallery] = useState(false);
  const [navParams, setNavParams] = useState<Record<string, unknown>>({});

  // Gift toast — a neighbouring kingdom sent us a care package (royal diplomacy).
  // We track the last acknowledged gift by its unix timestamp so it only pops
  // once per gift, and baseline to existing history on first run so old gifts
  // (from before the app was installed) never pop.
  const [giftSeenUnix, setGiftSeenUnix] = useState<number>(
    () => Number(localStorage.getItem(GIFT_SEEN_KEY) || 0)
  );
  const [discoverySeenUnix, setDiscoverySeenUnix] = useState<number>(
    () => Number(localStorage.getItem(DISCOVERY_SEEN_KEY) || 0)
  );
  const [dormantDismissed, setDormantDismissed] = useState(false);

  // Colony state managed here, provided via context
  const [colonyState, setColonyState] = useState<ColonyState>({
    snapshot: null,
    events: [],
    source: 'none',
    lastFetchMs: 0,
    colonyId,
    loading: true,
  });

  const pollerRef = useRef<Poller | null>(null);

  // Start/stop poller when colonyId changes
  useEffect(() => {
    if (!colonyId) {
      pollerRef.current?.stop();
      pollerRef.current = null;
      setColonyState(s => ({ ...s, snapshot: null, events: [], source: 'none', loading: false, colonyId: null }));
      return;
    }

    const poller = new Poller(colonyId, 15_000);
    pollerRef.current = poller;

    const unsub = poller.subscribe(state => {
      setColonyState(s => ({
        ...s,
        snapshot: state.snapshot ?? s.snapshot,
        // Poller now polls events too — keep them fresh so the gift toast can
        // surface proactively (don't clobber with the initial null state).
        events: state.events?.results ?? s.events,
        source: state.source as DataSource,
        lastFetchMs: state.lastFetchMs,
        colonyId,
        loading: false,
      }));
    });

    poller.start();

    // Initial events fetch
    fetchEvents(colonyId).then(r => {
      if (r) {
        setColonyState(s => ({ ...s, events: r.data.results }));
      }
    });

    return () => {
      unsub();
      poller.stop();
    };
  }, [colonyId]);

  const handleConnect = useCallback((id: string) => {
    setStoredColonyId(id);
    setColonyIdState(id);
  }, []);

  const handleDisconnect = useCallback(() => {
    clearConnection();
    pollerRef.current?.stop();
    pollerRef.current = null;
    setColonyIdState(null);
    setColonyState({
      snapshot: null,
      events: [],
      source: 'none',
      lastFetchMs: 0,
      colonyId: null,
      loading: false,
    });
    setShowSettings(false);
  }, []);

  const refreshEvents = useCallback(async (since?: number) => {
    if (!colonyId) return;
    const r = await fetchEvents(colonyId, since);
    if (r) {
      setColonyState(s => ({ ...s, events: r.data.results }));
    }
  }, [colonyId]);

  const actions: ColonyActions = useMemo(() => ({
    refreshEvents,
    setColonyId: handleConnect,
    disconnect: handleDisconnect,
  }), [refreshEvents, handleConnect, handleDisconnect]);

  const handleNavigate = useCallback((target: string, params?: Record<string, unknown>) => {
    if (target === 'settings') {
      setShowSettings(true);
      return;
    }
    if (target === 'fieldguide') {
      setNavParams(params || {});
      setShowFieldGuide(true);
      return;
    }
    if (target === 'gallery') {
      setShowGallery(true);
      return;
    }
    setTab(target as Tab);
    setNavParams(params || {});
    setShowSettings(false);
    setShowFieldGuide(false);
    setShowGallery(false);
  }, []);

  // Newest "care package from a neighbour" event, if any.
  const latestGift = useMemo<ColonyEvent | null>(() => {
    let best: ColonyEvent | null = null;
    for (const e of colonyState.events) {
      if (e.type === 'colony_event' && e.data?.kind === 'care_package_from_neighbours') {
        if (!best || e.unix > best.unix) best = e;
      }
    }
    return best;
  }, [colonyState.events]);

  // First run: baseline to existing history so we only pop for FUTURE gifts.
  useEffect(() => {
    if (latestGift && localStorage.getItem(GIFT_SEEN_KEY) === null) {
      localStorage.setItem(GIFT_SEEN_KEY, String(latestGift.unix));
      setGiftSeenUnix(latestGift.unix);
    }
  }, [latestGift]);

  const showGiftToast = !!latestGift && latestGift.unix > giftSeenUnix;

  const dismissGift = useCallback(() => {
    if (latestGift) {
      localStorage.setItem(GIFT_SEEN_KEY, String(latestGift.unix));
      setGiftSeenUnix(latestGift.unix);
    }
  }, [latestGift]);

  // Self-healing push: if the browser already granted permission and the
  // keeper wants notifications, silently (re)register the subscription +
  // pref with the server on every boot. Heals stale-PWA states where the
  // pref was saved locally but never reached the server.
  useEffect(() => {
    if (!colonyId) return;
    const pref = notificationManager.getPref();
    if (pref === 'off') return;
    if (typeof Notification === 'undefined' || Notification.permission !== 'granted') return;
    enablePushNotifications(colonyId, pref === 'milestones' ? 'milestones' : 'all');
  }, [colonyId]);

  // Newest critter discovery ("Dahlia found a beetle"), if any.
  const latestDiscovery = useMemo<ColonyEvent | null>(() => {
    let best: ColonyEvent | null = null;
    for (const e of colonyState.events) {
      if (e.type === 'discovery') {
        if (!best || e.unix > best.unix) best = e;
      }
    }
    return best;
  }, [colonyState.events]);

  useEffect(() => {
    if (latestDiscovery && localStorage.getItem(DISCOVERY_SEEN_KEY) === null) {
      localStorage.setItem(DISCOVERY_SEEN_KEY, String(latestDiscovery.unix));
      setDiscoverySeenUnix(latestDiscovery.unix);
    }
  }, [latestDiscovery]);

  const showDiscoveryToast = !!latestDiscovery && latestDiscovery.unix > discoverySeenUnix;

  const dismissDiscovery = useCallback(() => {
    if (latestDiscovery) {
      localStorage.setItem(DISCOVERY_SEEN_KEY, String(latestDiscovery.unix));
      setDiscoverySeenUnix(latestDiscovery.unix);
    }
  }, [latestDiscovery]);

  // "Eggs lie dormant" — driven by the live snapshot flag (the colony is at its
  // population cap with brood waiting). Using the snapshot rather than the one-shot
  // colony_event makes this reliable: it shows whenever the colony is full, and
  // re-arms once a slot frees and it fills again. (The event still drives the
  // closed-app push + diary entry server-side.)
  const eggsDormant = colonyState.snapshot?.population?.eggs_dormant ?? false;

  useEffect(() => {
    if (!eggsDormant) setDormantDismissed(false);  // re-arm when there's room again
  }, [eggsDormant]);

  const showDormantToast = eggsDormant && !dormantDismissed;
  const dismissDormant = useCallback(() => setDormantDismissed(true), []);

  // Module firmware: compare the latest published version to what the
  // connected colony reports, and offer a one-tap OTA (the ota_update command).
  const [latestFw, setLatestFw] = useState<number | null>(null);
  const [fwUpdating, setFwUpdating] = useState(false);

  useEffect(() => {
    if (!colonyId) { setLatestFw(null); return; }
    fetchLatestFirmware().then(v => { if (v) setLatestFw(v); });
  }, [colonyId]);

  const currentFw = colonyState.snapshot?.fw_version;
  const fwUpdateAvailable = !!latestFw && typeof currentFw === 'number' && latestFw > currentFw;

  useEffect(() => {
    // Module rebooted into the new version — clear the "updating" state.
    if (typeof currentFw === 'number' && latestFw && currentFw >= latestFw) setFwUpdating(false);
  }, [currentFw, latestFw]);

  const triggerFwUpdate = useCallback(async () => {
    if (!colonyId) return;
    setFwUpdating(true);
    await sendCommand(colonyId, 'ota_update', {});
  }, [colonyId]);

  const discoveryText = (() => {
    if (!latestDiscovery) return '';
    const critter = String((latestDiscovery.data as Record<string, unknown>)?.critter || 'critter');
    const finder = (latestDiscovery as unknown as { name?: string }).name || 'Someone';
    const emoji = critterEmoji(critter);
    return `${emoji} ${finder} found a ${critter}! Tap to see.`;
  })();

  // No colony — show empty state
  if (!colonyId) {
    return <Empty onConnected={handleConnect} />;
  }

  // Loading
  if (colonyState.loading && !colonyState.snapshot) {
    return (
      <div style={{
        background: HIVE.cream, minHeight: '100vh',
        display: 'flex', alignItems: 'center', justifyContent: 'center',
        color: HIVE.dimText, fontSize: SIZES.base,
      }}>
        Connecting to colony...
      </div>
    );
  }

  // No snapshot available (offline, never loaded)
  if (!colonyState.snapshot && !colonyState.loading) {
    return <Empty onConnected={handleConnect} />;
  }

  return (
    <AuthGate>
      <ColonyContext.Provider value={colonyState}>
        <ColonyActionsContext.Provider value={actions}>
          <PinsProvider>
            <PinsSync colonyId={colonyId} />
            <div style={{
              display: 'flex', flexDirection: 'column',
              height: '100vh', width: '100%', maxWidth: 430, margin: '0 auto',
              background: HIVE.cream, position: 'relative',
              overflow: 'hidden',
            }}>
              {/* Main content area */}
              <div style={{ flex: 1, overflowY: 'auto', overflowX: 'hidden', WebkitOverflowScrolling: 'touch' }}>
                {showSettings ? (
                  <Settings
                    onBack={() => setShowSettings(false)}
                    onDisconnect={handleDisconnect}
                    onReconnect={(id) => { handleConnect(id); setShowSettings(false); }}
                  />
                ) : showFieldGuide ? (
                  <FieldGuide
                    onBack={() => setShowFieldGuide(false)}
                    initialKind={navParams.kind as string | undefined}
                  />
                ) : showGallery ? (
                  <Gallery onBack={() => setShowGallery(false)} />
                ) : (
                  <>
                    {tab === 'home' && <Home onNavigate={handleNavigate} />}
                    {tab === 'characters' && (
                      <Characters
                        onNavigate={handleNavigate}
                        initialLilguyId={navParams.lilguyId as number | undefined}
                      />
                    )}
                    {tab === 'chambers' && <Chambers />}
                    {tab === 'diary' && <Diary />}
                    {tab === 'feedback' && <Feedback />}
                  </>
                )}
              </div>

              {/* Discovery toast — a conker found a visiting critter */}
              {showDiscoveryToast && (
                <button
                  onClick={() => {
                    // Straight to the critter's close-up, not the diary
                    const kind = String((latestDiscovery?.data as Record<string, unknown>)?.critter || '');
                    handleNavigate('fieldguide', kind ? { kind } : undefined);
                    dismissDiscovery();
                  }}
                  style={{
                    flexShrink: 0,
                    margin: '0 12px 8px',
                    padding: '12px 16px',
                    background: HIVE.leafGreen,
                    color: HIVE.white,
                    border: 'none',
                    borderRadius: 12,
                    fontSize: SIZES.sm,
                    fontWeight: 600,
                    cursor: 'pointer',
                    boxShadow: '0 2px 8px rgba(0,0,0,0.15)',
                    textAlign: 'left',
                  }}
                >
                  {discoveryText}
                </button>
              )}

              {/* Gift toast — a neighbouring kingdom sent a care package */}
              {showGiftToast && (
                <button
                  onClick={() => { handleNavigate('diary'); dismissGift(); }}
                  style={{
                    flexShrink: 0,
                    margin: '0 12px 8px',
                    padding: '12px 16px',
                    background: HIVE.green,
                    color: HIVE.white,
                    border: 'none',
                    borderRadius: 12,
                    fontSize: SIZES.sm,
                    fontWeight: 600,
                    cursor: 'pointer',
                    boxShadow: '0 2px 8px rgba(0,0,0,0.15)',
                    textAlign: 'left',
                  }}
                >
                  {'\u{1F381}'} A neighbouring kingdom sent a care package! Tap to see it.
                </button>
              )}

              {/* Population cap — eggs lie dormant until there's room */}
              {showDormantToast && (
                <button
                  onClick={dismissDormant}
                  style={{
                    flexShrink: 0,
                    margin: '0 12px 8px',
                    padding: '12px 16px',
                    background: HIVE.accent,
                    color: HIVE.white,
                    border: 'none',
                    borderRadius: 12,
                    fontSize: SIZES.sm,
                    fontWeight: 600,
                    cursor: 'pointer',
                    boxShadow: '0 2px 8px rgba(0,0,0,0.15)',
                    textAlign: 'left',
                  }}
                >
                  {'\u{1F95A}'} Your eggs lie dormant for now — more space is needed.
                </button>
              )}

              {/* Module firmware OTA — push new firmware to the hardware */}
              {fwUpdateAvailable && (
                <button
                  onClick={triggerFwUpdate}
                  disabled={fwUpdating}
                  style={{
                    flexShrink: 0,
                    margin: '0 12px 8px',
                    padding: '12px 16px',
                    background: HIVE.sky,
                    color: HIVE.ink,
                    border: 'none',
                    borderRadius: 12,
                    fontSize: SIZES.sm,
                    fontWeight: 600,
                    cursor: fwUpdating ? 'default' : 'pointer',
                    opacity: fwUpdating ? 0.7 : 1,
                    boxShadow: '0 2px 8px rgba(0,0,0,0.15)',
                    textAlign: 'left',
                  }}
                >
                  {fwUpdating
                    ? '\u{1F504} Updating the wee guys’ brains… they’ll blink and reboot'
                    : `\u{1F4E1} Module firmware v${latestFw} is ready — tap to update over WiFi`}
                </button>
              )}

              {/* Tab bar */}
              {!showSettings && (
                <nav style={{
                  display: 'flex',
                  borderTop: `1px solid ${HIVE.parchment}`,
                  background: HIVE.cream,
                  paddingBottom: 'env(safe-area-inset-bottom, 0)',
                  flexShrink: 0,
                }}>
                  {(Object.keys(TAB_LABELS) as Tab[]).map(t => (
                    <button
                      key={t}
                      onClick={() => { setTab(t); setNavParams({}); }}
                      style={{
                        flex: 1,
                        padding: '12px 0 14px',
                        minHeight: 64,
                        background: 'none',
                        border: 'none',
                        cursor: 'pointer',
                        fontSize: SIZES.xs,
                        fontWeight: tab === t ? 600 : 400,
                        color: tab === t ? HIVE.accent : HIVE.dimText,
                        textAlign: 'center',
                        borderTop: tab === t ? `3px solid ${HIVE.accent}` : '3px solid transparent',
                      }}
                    >
                      <div style={{ fontSize: 22, lineHeight: '26px', filter: tab === t ? 'none' : 'grayscale(0.6)' }}>
                        {TAB_ICONS[t]}
                      </div>
                      <div style={{ marginTop: 2 }}>{TAB_LABELS[t]}</div>
                    </button>
                  ))}
                </nav>
              )}
            </div>
          </PinsProvider>
        </ColonyActionsContext.Provider>
      </ColonyContext.Provider>
    </AuthGate>
  );
}

// Mirrors the pin list to the module (set_followed command) so followed
// conkers get a small gold star on the glass. Sends on change, once.
function PinsSync({ colonyId }: { colonyId: string | null }) {
  const { pins } = usePins();
  const lastSent = useRef<string>('');
  useEffect(() => {
    if (!colonyId) return;
    const ids = pins.slice(0, 8);  // firmware caps at 8 followed
    const key = JSON.stringify(ids);
    if (key === lastSent.current) return;
    lastSent.current = key;
    sendCommand(colonyId, 'set_followed', { ids });
  }, [pins, colonyId]);
  return null;
}
