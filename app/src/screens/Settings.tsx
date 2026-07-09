import { useState, useEffect } from 'react';
import { useColony, timeSince } from '../state/colony';
import { getStoredLanIp, getStoredColonyId, clearConnection, testLanConnection, setStoredLanIp, setStoredColonyId, fetchColonies, enablePushNotifications, disablePushNotifications, pushSupported, sendTestPush } from '../api/client';
import type { PushResult } from '../api/client';
import { Card } from '../components/Card';
import { getIdentity, resetColonyIdentity, AUTOSTART_KEY, WIPE_KEY, LAST_ACTIVE_KEY } from '../localModule';
import { HIVE, CONNECTION_COLORS } from '../theme/palette';
import { SIZES } from '../theme/fonts';

const NOTIF_KEY = 'hive_notification_pref';
type NotifPref = 'off' | 'milestones' | 'all';

const NOTIF_OPTIONS: { value: NotifPref; label: string; description: string }[] = [
  { value: 'off', label: 'Off', description: 'Silence.' },
  { value: 'milestones', label: 'Milestones only', description: 'The colony will speak when something matters.' },
  { value: 'all', label: 'All notable events', description: 'Hear everything the colony chooses to highlight.' },
];

function loadNotifPref(): NotifPref {
  return (localStorage.getItem(NOTIF_KEY) as NotifPref) || 'milestones';
}

function saveNotifPref(pref: NotifPref): void {
  localStorage.setItem(NOTIF_KEY, pref);
}

// Notification pref manager — the pref is sent with the push subscription,
// so the server tiers what it sends (milestones vs all + evening digest)
export const notificationManager = {
  getPref: loadNotifPref,
  setPref: saveNotifPref,
  // Future: requestPermission(), sendNotification(), etc.
};

interface SettingsProps {
  onBack: () => void;
  onDisconnect: () => void;
  onReconnect?: (colonyId: string) => void;
}

export function Settings({ onBack, onDisconnect, onReconnect }: SettingsProps) {
  const [showConnect, setShowConnect] = useState(false);
  const { snapshot, source, lastFetchMs, colonyId } = useColony();
  const [notifPref, setNotifPref] = useState<NotifPref>(loadNotifPref);
  // 'ok' only when an actual push subscription exists in this browser —
  // browser permission alone used to fake "notifications are on"
  const [pushState, setPushState] = useState<'idle' | 'working' | PushResult>('idle');
  const [testResult, setTestResult] = useState<string | null>(null);

  useEffect(() => {
    if (!pushSupported() || Notification.permission !== 'granted') return;
    navigator.serviceWorker.ready
      .then(reg => reg.pushManager.getSubscription())
      .then(sub => { if (sub) setPushState('ok'); })
      .catch(() => {});
  }, []);

  const handleTestPush = async () => {
    if (!colonyId) return;
    setTestResult('Sending…');
    const n = await sendTestPush(colonyId);
    setTestResult(n === null
      ? 'Couldn’t reach the server — try again.'
      : n === 0
        ? 'The server has no registered devices — re-pick a preference below.'
        : `Sent to ${n} device${n === 1 ? '' : 's'} — should buzz any second.`);
  };

  const handleEnablePush = async () => {
    if (!colonyId) return;
    setPushState('working');
    setPushState(await enablePushNotifications(
      colonyId, notifPref === 'milestones' ? 'milestones' : 'all'));
  };

  const lanIp = getStoredLanIp();
  const storedColonyId = getStoredColonyId();

  // The pref is real now: it re-registers the subscription server-side so
  // the VPS knows which tier of pushes (and the 8pm digest) to send here
  const handleNotifChange = async (pref: NotifPref) => {
    setNotifPref(pref);
    saveNotifPref(pref);
    if (!colonyId) return;
    setPushState('working');
    if (pref === 'off') {
      setPushState(await disablePushNotifications(colonyId));
    } else {
      setPushState(await enablePushNotifications(colonyId, pref));
    }
  };

  const handleDisconnect = () => {
    clearConnection();
    onDisconnect();
  };

  // This install is running its own on-phone colony (not viewing a physical one).
  const isOwnColony = !!colonyId && colonyId === getIdentity().colony_id;

  // Start over: mint a new install identity (→ a new seed → a new queen + cast),
  // then reload so a clean WASM instance founds it. Destructive — the current
  // colony is gone (there's no cross-reload persistence yet anyway).
  const handleFreshColony = () => {
    if (!window.confirm(
      'Start a completely new colony? This one — its characters, diary and history — will be gone for good.')) return;
    resetColonyIdentity();
    localStorage.setItem(AUTOSTART_KEY, '1');  // Empty.tsx boots the new colony on load
    localStorage.setItem(WIPE_KEY, '1');       // clear persisted /colony so it founds fresh, not restores
    localStorage.removeItem(LAST_ACTIVE_KEY);  // no catch-up from the old colony's clock
    clearConnection();
    window.location.reload();
  };

  // About colony data
  const founded = snapshot?.founded_unix
    ? new Date(snapshot.founded_unix * 1000).toLocaleDateString()
    : null;
  const ageInDays = snapshot?.founded_unix && snapshot?.now_unix
    ? ((snapshot.now_unix - snapshot.founded_unix) / 86400).toFixed(0)
    : null;

  return (
    <div style={{ background: HIVE.cream, minHeight: '100%', padding: '0 16px 100px' }}>
      {/* Header */}
      <div style={{ padding: '16px 0 8px' }}>
        <button onClick={onBack} style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: SIZES.base, color: HIVE.accent }}>
          &larr; Back
        </button>
      </div>
      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: HIVE.ink, margin: '0 0 16px' }}>
        Settings
      </h1>

      {/* Notifications */}
      <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: HIVE.dimText, textTransform: 'uppercase', letterSpacing: 1, marginBottom: 8 }}>
        Notifications
      </div>
      <Card style={{ background: '#F5ECDD', marginBottom: 8 }}>
        <div style={{ fontSize: SIZES.sm, color: HIVE.ink, marginBottom: 8 }}>
          Get a buzz on your phone when something happens — a bug discovered, a gift
          from next door — even when the app is closed.
        </div>
        {pushState === 'ok' ? (
          <div>
            <div style={{ fontSize: SIZES.sm, color: HIVE.green, fontWeight: 600 }}>
              ✓ Phone notifications are on
            </div>
            <button
              onClick={handleTestPush}
              style={{
                marginTop: 8, padding: '6px 14px', borderRadius: 10,
                border: `1.5px solid ${HIVE.accent}`, background: 'none',
                color: HIVE.accent, fontSize: SIZES.sm, fontWeight: 600, cursor: 'pointer',
              }}
            >
              Send a test notification
            </button>
            {testResult && (
              <div style={{ fontSize: SIZES.xs, color: HIVE.dimText, marginTop: 6 }}>
                {testResult}
              </div>
            )}
          </div>
        ) : !pushSupported() ? (
          <div style={{ fontSize: SIZES.sm, color: HIVE.dimText, fontStyle: 'italic' }}>
            On iPhone, add this app to your Home Screen first (Share → Add to Home
            Screen), then come back here to switch it on.
          </div>
        ) : (
          <button
            onClick={handleEnablePush}
            disabled={pushState === 'working'}
            style={{
              padding: '8px 16px', borderRadius: 10, border: 'none',
              background: HIVE.accent, color: HIVE.white, fontSize: SIZES.sm,
              fontWeight: 600, cursor: 'pointer', opacity: pushState === 'working' ? 0.6 : 1,
            }}
          >
            {pushState === 'working' ? 'Enabling…' : 'Enable phone notifications'}
          </button>
        )}
        {pushState === 'denied' && (
          <div style={{ fontSize: SIZES.xs, color: HIVE.alert, marginTop: 6 }}>
            Notifications are blocked — turn them on for this site in your browser/phone settings.
          </div>
        )}
        {pushState === 'error' && (
          <div style={{ fontSize: SIZES.xs, color: HIVE.alert, marginTop: 6 }}>
            Couldn’t enable just now — give it another tap.
          </div>
        )}
      </Card>
      <Card style={{ background: '#F5ECDD' }}>
        {NOTIF_OPTIONS.map(opt => (
          <label
            key={opt.value}
            style={{
              display: 'flex', alignItems: 'flex-start', gap: 10,
              padding: '8px 0', cursor: 'pointer',
              borderBottom: opt.value !== 'all' ? `1px solid ${HIVE.parchment}` : undefined,
            }}
          >
            <input
              type="radio"
              name="notif"
              checked={notifPref === opt.value}
              onChange={() => handleNotifChange(opt.value)}
              style={{ marginTop: 3, accentColor: HIVE.accent }}
            />
            <div>
              <div style={{ fontSize: SIZES.base, fontWeight: 500, color: HIVE.ink }}>
                {opt.label}
              </div>
              <div style={{ fontSize: SIZES.sm, color: HIVE.dimText, fontStyle: 'italic' }}>
                {opt.description}
              </div>
            </div>
          </label>
        ))}
      </Card>

      {/* Connection */}
      <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: HIVE.dimText, textTransform: 'uppercase', letterSpacing: 1, margin: '16px 0 8px' }}>
        Connection
      </div>
      <Card style={{ background: '#F5ECDD' }}>
        <InfoRow label="Colony ID" value={storedColonyId || colonyId || 'Unknown'} />
        <InfoRow label="Queen LAN IP" value={lanIp || 'Not configured'} />
        <InfoRow label="VPS" value="hive.campbell.fish" />
        <InfoRow
          label="Data source"
          value={
            <span style={{ display: 'inline-flex', alignItems: 'center', gap: 6 }}>
              <span style={{
                width: 8, height: 8, borderRadius: '50%',
                background: CONNECTION_COLORS[source], display: 'inline-block',
              }} />
              {source === 'lan' ? 'LAN (freshest)' :
               source === 'vps' ? 'VPS (cloud cache)' :
               source === 'cache' ? 'Cached (offline)' : 'Not connected'}
            </span>
          }
        />
        <InfoRow label="Last update" value={timeSince(lastFetchMs)} />

        <div style={{ display: 'flex', gap: 8, marginTop: 12 }}>
          <button
            onClick={() => setShowConnect(true)}
            style={{
              flex: 1, padding: '8px 0', borderRadius: 10,
              border: `1px solid ${HIVE.accent}`, background: 'transparent',
              color: HIVE.accent, fontSize: SIZES.sm, cursor: 'pointer',
            }}
          >
            Change module
          </button>
          <button
            onClick={handleDisconnect}
            style={{
              flex: 1, padding: '8px 0', borderRadius: 10,
              border: `1px solid ${HIVE.alert}`, background: 'transparent',
              color: HIVE.alert, fontSize: SIZES.sm, cursor: 'pointer',
            }}
          >
            Disconnect
          </button>
        </div>

        {showConnect && (
          <ConnectInline
            onClose={() => setShowConnect(false)}
            onConnected={(id) => { setShowConnect(false); onReconnect?.(id); }}
          />
        )}
      </Card>

      {/* Fresh colony — only for a colony running on THIS phone */}
      {isOwnColony && (
        <>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: HIVE.dimText, textTransform: 'uppercase', letterSpacing: 1, margin: '16px 0 8px' }}>
            This colony
          </div>
          <Card style={{ background: '#F5ECDD' }}>
            <div style={{ fontSize: SIZES.sm, color: HIVE.ink, marginBottom: 10 }}>
              Running on this phone. Start over and a brand-new queen and colony
              take its place — the current one won't come back.
            </div>
            <button
              onClick={handleFreshColony}
              style={{
                width: '100%', padding: '10px 0', borderRadius: 10,
                border: `1px solid ${HIVE.alert}`, background: 'transparent',
                color: HIVE.alert, fontSize: SIZES.sm, fontWeight: 600, cursor: 'pointer',
              }}
            >
              Start a fresh colony
            </button>
          </Card>
        </>
      )}

      {/* About this colony */}
      {snapshot && (
        <>
          <div style={{ fontSize: SIZES.xs, fontWeight: 600, color: HIVE.dimText, textTransform: 'uppercase', letterSpacing: 1, margin: '16px 0 8px' }}>
            About this colony
          </div>
          <Card style={{ background: '#F5ECDD' }}>
            {founded && <InfoRow label="Founded" value={founded} />}
            {ageInDays && <InfoRow label="Age" value={`${ageInDays} days`} />}
            <InfoRow label="Population" value={String(snapshot.population.alive)} />
            <InfoRow label="Total workers (ever)" value={String(snapshot.population.alive + snapshot.population.dead_total)} />

            <a
              href={`https://hive.campbell.fish/api/v1/colonies/${storedColonyId || colonyId}`}
              target="_blank"
              rel="noopener noreferrer"
              style={{
                display: 'block', marginTop: 12,
                fontSize: SIZES.sm, color: HIVE.accent,
              }}
            >
              View raw data &rarr;
            </a>
          </Card>
        </>
      )}
    </div>
  );
}

function InfoRow({ label, value }: { label: string; value: string | React.ReactNode }) {
  return (
    <div style={{ display: 'flex', justifyContent: 'space-between', padding: '4px 0', fontSize: SIZES.sm }}>
      <span style={{ color: HIVE.dimText }}>{label}</span>
      <span style={{ color: HIVE.ink, fontWeight: 500, textAlign: 'right' }}>{value}</span>
    </div>
  );
}

function ConnectInline({ onClose, onConnected }: {
  onClose: () => void;
  onConnected: (colonyId: string) => void;
}) {
  const [lanIp, setLanIp] = useState('');
  const [colonyIdInput, setColonyIdInput] = useState('');
  const [status, setStatus] = useState<'idle' | 'testing' | 'error'>('idle');
  const [errorMsg, setErrorMsg] = useState('');

  const handleConnect = async () => {
    setStatus('testing');
    if (lanIp) {
      const ok = await testLanConnection(lanIp);
      if (ok) setStoredLanIp(lanIp);
    }
    let id = colonyIdInput;
    if (!id) {
      const list = await fetchColonies();
      if (list && list.colonies.length > 0) {
        const sorted = [...list.colonies].sort((a, b) => b.last_snapshot_unix - a.last_snapshot_unix);
        id = sorted[0].colony_id;
      }
    }
    if (!id) {
      setStatus('error');
      setErrorMsg('Could not detect a colony.');
      return;
    }
    setStoredColonyId(id);
    onConnected(id);
  };

  return (
    <div style={{ marginTop: 12, padding: 12, background: HIVE.cream, borderRadius: 10 }}>
      <input
        type="text" value={lanIp} onChange={e => setLanIp(e.target.value)}
        placeholder="Queen LAN IP (optional)"
        style={{ width: '100%', padding: '6px 10px', borderRadius: 6, border: `1px solid ${HIVE.sand}`, fontSize: SIZES.sm, marginBottom: 8, background: HIVE.white }}
      />
      <input
        type="text" value={colonyIdInput} onChange={e => setColonyIdInput(e.target.value)}
        placeholder="Colony ID (auto-detect)"
        style={{ width: '100%', padding: '6px 10px', borderRadius: 6, border: `1px solid ${HIVE.sand}`, fontSize: SIZES.sm, marginBottom: 8, background: HIVE.white }}
      />
      {status === 'error' && <div style={{ fontSize: SIZES.xs, color: HIVE.alert, marginBottom: 6 }}>{errorMsg}</div>}
      <div style={{ display: 'flex', gap: 8 }}>
        <button onClick={onClose} style={{ flex: 1, padding: '6px 0', borderRadius: 8, border: `1px solid ${HIVE.sand}`, background: 'transparent', color: HIVE.soil, fontSize: SIZES.sm, cursor: 'pointer' }}>Cancel</button>
        <button onClick={handleConnect} disabled={status === 'testing'} style={{ flex: 1, padding: '6px 0', borderRadius: 8, border: 'none', background: HIVE.accent, color: HIVE.white, fontSize: SIZES.sm, fontWeight: 600, cursor: 'pointer', opacity: status === 'testing' ? 0.6 : 1 }}>
          {status === 'testing' ? 'Testing...' : 'Connect'}
        </button>
      </div>
    </div>
  );
}
