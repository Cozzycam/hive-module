import { useState, useEffect } from 'react';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { testLanConnection, setStoredLanIp, setStoredColonyId, fetchColonies } from '../api/client';
import { localModule } from '../localModule';

function agoLabel(unix: number): string {
  if (!unix) return 'never seen';
  const s = Math.max(0, Math.floor(Date.now() / 1000) - unix);
  if (s < 90) return 'active just now';
  if (s < 3600) return `active ${Math.round(s / 60)}m ago`;
  if (s < 86400) return `active ${Math.round(s / 3600)}h ago`;
  return `active ${Math.round(s / 86400)}d ago`;
}

const DEMO_COLONY_ID = '3b5ddabf8c9459bd602b3c7a';

interface EmptyProps {
  onConnected: (colonyId: string) => void;
}

export function Empty({ onConnected }: EmptyProps) {
  const [showModal, setShowModal] = useState(false);
  const [starting, setStarting] = useState(false);

  // Boot a queen module on THIS phone: run the sim locally, enrol its own
  // colony, push a first snapshot to the hive, then connect the app to it.
  async function runLocalModule() {
    setStarting(true);
    try {
      await localModule.start();
      await localModule.pushNow();     // ensure the colony exists on the VPS before connecting
      onConnected(localModule.identity.colony_id);
    } catch (e) {
      console.error('[empty] local module failed', e);
      setStarting(false);
    }
  }

  return (
    <div style={{
      background: HIVE.cream,
      minHeight: '100vh',
      display: 'flex',
      flexDirection: 'column',
      alignItems: 'center',
      justifyContent: 'center',
      padding: 32,
      textAlign: 'center',
    }}>
      {/* Ambient illustration — a conker, as it appears on the modules */}
      <div style={{ marginBottom: 32 }}>
        <img
          src="/app/conker.png"
          alt=""
          width={96}
          height={96}
          style={{ imageRendering: 'pixelated', display: 'block' }}
        />
      </div>

      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: HIVE.ink, margin: '0 0 12px' }}>
        The colony hasn't begun yet.
      </h1>

      <p style={{ fontSize: SIZES.base, color: HIVE.dimText, maxWidth: 320, lineHeight: 1.5, marginBottom: 32 }}>
        This app is a window onto a Hive Module. The colony lives on the modules
        themselves — connect to a queen module to see your colony here.
      </p>

      <button
        onClick={runLocalModule}
        disabled={starting}
        style={{
          background: HIVE.accent,
          color: HIVE.white,
          border: 'none',
          borderRadius: 24,
          padding: '12px 28px',
          fontSize: SIZES.base,
          fontWeight: 700,
          cursor: starting ? 'default' : 'pointer',
          marginBottom: 12,
          opacity: starting ? 0.7 : 1,
        }}
      >
        {starting ? 'starting your colony…' : 'Run a queen module on this phone'}
      </button>

      <button
        onClick={() => setShowModal(true)}
        style={{
          background: 'none',
          color: HIVE.accent,
          border: `1px solid ${HIVE.accent}`,
          borderRadius: 24,
          padding: '10px 24px',
          fontSize: SIZES.sm,
          fontWeight: 600,
          cursor: 'pointer',
          marginBottom: 16,
        }}
      >
        Connect to a physical module
      </button>

      <button
        onClick={() => onConnected(DEMO_COLONY_ID)}
        style={{
          background: 'none',
          border: 'none',
          color: HIVE.accent,
          fontSize: SIZES.sm,
          cursor: 'pointer',
          textDecoration: 'underline',
        }}
      >
        Or browse the demo colony
      </button>

      {showModal && (
        <ConnectModal
          onClose={() => setShowModal(false)}
          onConnected={onConnected}
        />
      )}
    </div>
  );
}

function ConnectModal({ onClose, onConnected }: {
  onClose: () => void;
  onConnected: (colonyId: string) => void;
}) {
  const [colonies, setColonies] = useState<{ colony_id: string; last_snapshot_unix: number }[] | null>(null);
  const [showAdvanced, setShowAdvanced] = useState(false);
  const [lanIp, setLanIp] = useState('');

  useEffect(() => {
    fetchColonies().then(r => {
      const list = r?.colonies ? [...r.colonies] : [];
      list.sort((a, b) => b.last_snapshot_unix - a.last_snapshot_unix);
      setColonies(list);
    });
  }, []);

  const pick = async (id: string) => {
    if (lanIp && await testLanConnection(lanIp)) setStoredLanIp(lanIp);
    setStoredColonyId(id);
    onConnected(id);
  };

  return (
    <div style={{
      position: 'fixed', inset: 0,
      background: 'rgba(0,0,0,0.4)',
      display: 'flex', alignItems: 'center', justifyContent: 'center',
      zIndex: 1000, padding: 16,
    }}
      onClick={onClose}
    >
      <div
        onClick={e => e.stopPropagation()}
        style={{
          background: HIVE.white,
          borderRadius: 20,
          padding: 24,
          maxWidth: 360,
          width: '100%',
          maxHeight: '80vh',
          overflowY: 'auto',
        }}
      >
        <h2 style={{ fontSize: SIZES.lg, fontWeight: 700, color: HIVE.ink, margin: '0 0 4px' }}>
          Choose your colony
        </h2>
        <p style={{ fontSize: SIZES.sm, color: HIVE.dimText, margin: '0 0 16px' }}>
          The name is shown on your module's screen, in the bar at the top.
        </p>

        {colonies === null ? (
          <div style={{ fontSize: SIZES.sm, color: HIVE.dimText, padding: '12px 0' }}>
            Looking for colonies…
          </div>
        ) : colonies.length === 0 ? (
          <div style={{ fontSize: SIZES.sm, color: HIVE.dimText, padding: '12px 0' }}>
            No colonies found yet — give the module a minute after setup.
          </div>
        ) : (
          colonies.map(c => (
            <button
              key={c.colony_id}
              onClick={() => pick(c.colony_id)}
              style={{
                display: 'block', width: '100%', textAlign: 'left',
                background: HIVE.cream, border: `1px solid ${HIVE.sand}`,
                borderRadius: 12, padding: '10px 14px', marginBottom: 8,
                cursor: 'pointer',
              }}
            >
              <div style={{ fontSize: SIZES.base, fontWeight: 600, color: HIVE.ink }}>
                {c.colony_id}
              </div>
              <div style={{ fontSize: SIZES.xs, color: HIVE.dimText }}>
                {agoLabel(c.last_snapshot_unix)}
              </div>
            </button>
          ))
        )}

        <button
          onClick={() => setShowAdvanced(!showAdvanced)}
          style={{
            background: 'none', border: 'none', color: HIVE.dimText,
            fontSize: SIZES.xs, cursor: 'pointer', padding: '8px 0',
            textDecoration: 'underline',
          }}
        >
          {showAdvanced ? 'hide advanced' : 'advanced (local IP)'}
        </button>
        {showAdvanced && (
          <input
            type="text"
            value={lanIp}
            onChange={e => setLanIp(e.target.value)}
            placeholder="Queen's local IP (optional)"
            style={{
              width: '100%', boxSizing: 'border-box', padding: '8px 12px', borderRadius: 8,
              border: `1px solid ${HIVE.sand}`, fontSize: SIZES.base,
              background: HIVE.cream, marginBottom: 8,
            }}
          />
        )}

        <button
          onClick={onClose}
          style={{
            width: '100%', padding: '10px 0', borderRadius: 12,
            border: `1px solid ${HIVE.sand}`, background: 'transparent',
            color: HIVE.soil, fontSize: SIZES.base, cursor: 'pointer', marginTop: 4,
          }}
        >
          Cancel
        </button>
      </div>
    </div>
  );
}
