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
import Module from './screens/Module';
import { Her } from './screens/Her';
import { Crown } from './screens/Crown';
import { About } from './screens/About';
import { localModule, getIdentity } from './localModule';
import { Empty } from './screens/Empty';
import { Settings, notificationManager } from './screens/Settings';
import { critterEmoji } from './data/critters';
import { loadAnnals } from './data/chronicle';
import { FieldGuide } from './screens/FieldGuide';
import { Gallery } from './screens/Gallery';
import { Saga, type ChronicleSection } from './screens/Saga';
import { HIVE } from './theme/palette';
import { SIZES } from './theme/fonts';
import type { ColonyEvent, DataSource } from './api/types';

type Tab = 'home' | 'module' | 'characters' | 'chambers' | 'diary' | 'feedback' | 'her' | 'about' | 'guide';

// Colony mode (a physical module, or a founded colony): the full app.
const COLONY_TABS: Tab[] = ['home', 'module', 'characters', 'chambers', 'diary', 'feedback'];
// Tamagotchi mode (raising a single princess, pre-coronation): a calm, slimmed shell.
const TAMAGOTCHI_TABS: Tab[] = ['her', 'about', 'diary', 'guide', 'feedback'];

const TAB_LABELS: Record<Tab, string> = {
  home: 'Home',
  module: 'Module',
  characters: 'Characters',
  chambers: 'Chambers',
  diary: 'Diary',
  feedback: 'Feedback',
  her: 'Nest',
  about: 'About',
  guide: 'Guide',
};

const TAB_ICONS: Record<Tab, string> = {
  home: '\u{1F3E0}',
  module: '\u{1F41C}',   // ant — your own live queen module
  characters: '\u{1F330}',
  chambers: '\u{1F9E9}',
  diary: '\u{1F4D6}',
  feedback: '\u{1F4AC}',
  her: '\u{1FABA}',      // nest with egg — where she's raised
  about: '\u{1F338}',    // blossom — who she is
  guide: '\u{1F50D}',    // magnifier — field guide
};

// localStorage key: unix of the most recent neighbour gift the user has seen.
const GIFT_SEEN_KEY = 'hive_last_gift_seen_unix';
// localStorage key: unix of the most recent critter discovery the user has seen.
const DISCOVERY_SEEN_KEY = 'hive_last_discovery_seen_unix';
// localStorage key: unix of the most recent recorded deed the user has seen.
const DEED_SEEN_KEY = 'hive_last_deed_seen_unix';

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
  const [showCrown, setShowCrown] = useState(false);
  const [showFieldGuide, setShowFieldGuide] = useState(false);
  const [showGallery, setShowGallery] = useState(false);
  const [showSaga, setShowSaga] = useState(false);
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
  const [deedSeenUnix, setDeedSeenUnix] = useState<number>(
    () => Number(localStorage.getItem(DEED_SEEN_KEY) || 0)
  );
  const [dormantDismissed, setDormantDismissed] = useState(false);

  // The initial 1000-deep events fetch, kept aside for seeding the annals
  // register — the poller clobbers colonyState.events with shallow windows,
  // and the write-once register must never seed from one of those.
  const [deepEvents, setDeepEvents] = useState<ColonyEvent[] | null>(null);
  const annalsSeededRef = useRef(false);

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

    // Initial events fetch — deep, because it seeds the annals register:
    // the write-once fold must see full history before firsts and counter
    // watermarks are allowed to persist (chronicle.ts loadAnnals).
    annalsSeededRef.current = false;
    setDeepEvents(null);
    fetchEvents(colonyId, 0, 1000).then(r => {
      if (r) {
        setColonyState(s => ({ ...s, events: r.data.results }));
        setDeepEvents(r.data.results);
      }
    });

    return () => {
      unsub();
      poller.stop();
    };
  }, [colonyId]);

  // If this install is connected to its OWN on-phone colony, keep the local
  // module running (ticking + pushing to the VPS) whenever the app is open,
  // regardless of which tab is showing.
  useEffect(() => {
    if (colonyId && colonyId === getIdentity().colony_id) {
      localModule.start().catch(e => console.error('[app] local module start', e));
    }
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
    if (target === 'crown') {
      setShowCrown(true);
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
    if (target === 'saga') {
      setNavParams(params || {});
      setShowSaga(true);
      return;
    }
    setTab(target as Tab);
    setNavParams(params || {});
    setShowSettings(false);
    setShowCrown(false);
    setShowFieldGuide(false);
    setShowGallery(false);
    setShowSaga(false);
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

  // Seed the annals register from the deep window, once per connection —
  // needs the snapshot too (truncation honesty and census deeds read it).
  // Until this has run, every loadAnnals call below is view-only.
  useEffect(() => {
    if (annalsSeededRef.current || !colonyId || !deepEvents || !colonyState.snapshot) return;
    loadAnnals(colonyId, deepEvents, colonyState.snapshot, { seed: true });
    annalsSeededRef.current = true;
  }, [colonyId, deepEvents, colonyState.snapshot]);

  // The Annals: fold the live window into the register so newly recorded
  // deeds can toast. Long Memory deeds never toast — grief is record, not
  // news. At most one toast is visible; the newest page wins.
  const annals = useMemo(
    () => (colonyId && colonyState.snapshot
      ? loadAnnals(colonyId, colonyState.events, colonyState.snapshot)
      : null),
    [colonyId, colonyState.events, colonyState.snapshot]);

  const latestDeed = useMemo(() => {
    if (!annals) return null;
    let best: { id: string; title: string; unix: number } | null = null;
    for (const dd of annals.deeds) {
      if (dd.state !== 'recorded' || dd.arc === 'memory' || !dd.unix) continue;
      if (!best || dd.unix > best.unix) best = { id: dd.id, title: dd.title, unix: dd.unix };
    }
    return best;
  }, [annals]);

  // First run: baseline to existing history — connecting to a mature colony
  // must produce zero toast avalanche.
  useEffect(() => {
    if (latestDeed && localStorage.getItem(DEED_SEEN_KEY) === null) {
      localStorage.setItem(DEED_SEEN_KEY, String(latestDeed.unix));
      setDeedSeenUnix(latestDeed.unix);
    }
  }, [latestDeed]);

  const showDeedToast = !!latestDeed && latestDeed.unix > deedSeenUnix;

  const dismissDeed = useCallback(() => {
    if (latestDeed) {
      localStorage.setItem(DEED_SEEN_KEY, String(latestDeed.unix));
      setDeedSeenUnix(latestDeed.unix);
    }
  }, [latestDeed]);

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

  // Tamagotchi mode: this install's OWN colony while she's still a single raised
  // princess (no queen yet). A physical module, or a colony she's founded, is the
  // full "colony mode" app. She runs in-process (localModule), so treat the phone's
  // own colony as a tamagotchi even before the first VPS snapshot lands — queen_name
  // is only ever set once she's actually been crowned.
  const ownColony = colonyId === getIdentity().colony_id;
  const isTamagotchi = ownColony && (!colonyState.snapshot || !colonyState.snapshot.queen_name);

  // Keep the active tab valid for the current mode (default to Her in tamagotchi).
  useEffect(() => {
    if (isTamagotchi && !TAMAGOTCHI_TABS.includes(tab)) setTab('her');
    else if (!isTamagotchi && !COLONY_TABS.includes(tab)) setTab('home');
  }, [isTamagotchi, tab]);

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

  // Loading — but the phone's OWN princess runs in-process; show her straight
  // away rather than waiting on a VPS round-trip.
  if (colonyState.loading && !colonyState.snapshot && !ownColony) {
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

  // No snapshot available (offline, never loaded) — again, the own princess
  // doesn't need one (the other tabs fill in as the VPS syncs).
  if (!colonyState.snapshot && !colonyState.loading && !ownColony) {
    return <Empty onConnected={handleConnect} />;
  }

  return (
    <AuthGate>
      <ColonyContext.Provider value={colonyState}>
        <ColonyActionsContext.Provider value={actions}>
          <PinsProvider key={colonyId} colonyId={colonyId}>
            <PinsSync colonyId={colonyId} />
            <div style={{
              display: 'flex', flexDirection: 'column',
              height: '100vh', width: '100%', maxWidth: 430, margin: '0 auto',
              background: HIVE.cream, position: 'relative',
              overflow: 'hidden',
            }}>
              {/* Main content area */}
              <div style={{ flex: 1, overflowY: 'auto', overflowX: 'hidden', WebkitOverflowScrolling: 'touch' }}>
                {showCrown ? (
                  <Crown onBack={() => setShowCrown(false)} />
                ) : showSettings ? (
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
                ) : showSaga ? (
                  <Saga
                    key={String(navParams.deedId ?? '')}
                    onBack={() => setShowSaga(false)}
                    initialSection={navParams.section as ChronicleSection | undefined}
                    initialDeedId={navParams.deedId as string | undefined}
                  />
                ) : (
                  <>
                    {tab === 'home' && <Home onNavigate={handleNavigate} />}
                    {tab === 'module' && colonyId === getIdentity().colony_id && <Module />}
                    {tab === 'characters' && (
                      <Characters
                        onNavigate={handleNavigate}
                        initialLilguyId={navParams.lilguyId as number | undefined}
                      />
                    )}
                    {tab === 'chambers' && <Chambers />}
                    {tab === 'diary' && <Diary />}
                    {tab === 'feedback' && <Feedback />}
                    {/* Tamagotchi mode */}
                    {tab === 'her' && <Her onNavigate={handleNavigate} />}
                    {tab === 'about' && <About />}
                    {tab === 'guide' && <FieldGuide onBack={() => setTab('her')} />}
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

              {/* Deed toast — a page of the Annals is written */}
              {showDeedToast && (
                <button
                  onClick={() => {
                    handleNavigate('saga', { section: 'deeds', deedId: latestDeed?.id });
                    dismissDeed();
                  }}
                  style={{
                    flexShrink: 0,
                    margin: '0 12px 8px',
                    padding: '12px 16px',
                    background: HIVE.bark,
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
                  {'\u{1F4DC}'} A page of the Annals is written: {latestDeed?.title}. Tap to read.
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
              {!showSettings && !showCrown && (
                <nav style={{
                  display: 'flex',
                  borderTop: `1px solid ${HIVE.parchment}`,
                  background: HIVE.cream,
                  paddingBottom: 'env(safe-area-inset-bottom, 0)',
                  flexShrink: 0,
                }}>
                  {(isTamagotchi ? TAMAGOTCHI_TABS : COLONY_TABS)
                    // The Module tab is only for a colony running ON this phone;
                    // a physical module already exists, so hide it there.
                    .filter(t => t !== 'module' || colonyId === getIdentity().colony_id)
                    .map(t => (
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
