import { useState, useEffect, useCallback, useRef, useMemo } from 'react';
import { ColonyContext, ColonyActionsContext, type ColonyState, type ColonyActions } from './state/colony';
import { PinsProvider } from './state/pins';
import { Poller } from './api/poller';
import { getStoredColonyId, setStoredColonyId, fetchEvents, clearConnection } from './api/client';
import { Home } from './screens/Home';
import { Characters } from './screens/Characters';
import { Chambers } from './screens/Chambers';
import { Diary } from './screens/Diary';
import { Feedback } from './screens/Feedback';
import { Empty } from './screens/Empty';
import { Settings } from './screens/Settings';
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
  const [navParams, setNavParams] = useState<Record<string, unknown>>({});

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
    setTab(target as Tab);
    setNavParams(params || {});
    setShowSettings(false);
  }, []);

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
                        padding: '10px 0 8px',
                        background: 'none',
                        border: 'none',
                        cursor: 'pointer',
                        fontSize: SIZES.xs,
                        fontWeight: tab === t ? 600 : 400,
                        color: tab === t ? HIVE.accent : HIVE.dimText,
                        textAlign: 'center',
                        borderTop: tab === t ? `2px solid ${HIVE.accent}` : '2px solid transparent',
                      }}
                    >
                      {TAB_LABELS[t]}
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
