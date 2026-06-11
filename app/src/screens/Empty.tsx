import { useState } from 'react';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';
import { testLanConnection, setStoredLanIp, setStoredColonyId, fetchColonies } from '../api/client';

const DEMO_COLONY_ID = '3b5ddabf8c9459bd602b3c7a';

interface EmptyProps {
  onConnected: (colonyId: string) => void;
}

export function Empty({ onConnected }: EmptyProps) {
  const [showModal, setShowModal] = useState(false);

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
        onClick={() => setShowModal(true)}
        style={{
          background: HIVE.accent,
          color: HIVE.white,
          border: 'none',
          borderRadius: 24,
          padding: '12px 28px',
          fontSize: SIZES.base,
          fontWeight: 600,
          cursor: 'pointer',
          marginBottom: 16,
        }}
      >
        Connect to a module
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
  const [lanIp, setLanIp] = useState('');
  const [colonyId, setColonyId] = useState('');
  const [status, setStatus] = useState<'idle' | 'testing' | 'error'>('idle');
  const [errorMsg, setErrorMsg] = useState('');

  const handleConnect = async () => {
    setStatus('testing');
    setErrorMsg('');

    if (lanIp) {
      const ok = await testLanConnection(lanIp);
      if (ok) {
        setStoredLanIp(lanIp);
      }
    }

    // Try to auto-detect colony ID (most recently updated)
    let resolvedColonyId = colonyId;
    if (!resolvedColonyId) {
      const colonies = await fetchColonies();
      if (colonies && colonies.colonies.length > 0) {
        const sorted = [...colonies.colonies].sort((a, b) => b.last_snapshot_unix - a.last_snapshot_unix);
        resolvedColonyId = sorted[0].colony_id;
      }
    }

    if (!resolvedColonyId) {
      setStatus('error');
      setErrorMsg('Could not detect a colony. Enter the colony ID manually, or check the connection.');
      return;
    }

    setStoredColonyId(resolvedColonyId);
    onConnected(resolvedColonyId);
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
        }}
      >
        <h2 style={{ fontSize: SIZES.lg, fontWeight: 700, color: HIVE.ink, margin: '0 0 16px' }}>
          Connect to a Module
        </h2>

        <label style={{ display: 'block', marginBottom: 12 }}>
          <div style={{ fontSize: SIZES.sm, color: HIVE.dimText, marginBottom: 4 }}>
            Queen's local IP (optional)
          </div>
          <input
            type="text"
            value={lanIp}
            onChange={e => setLanIp(e.target.value)}
            placeholder="e.g. 192.168.1.42"
            style={{
              width: '100%', padding: '8px 12px', borderRadius: 8,
              border: `1px solid ${HIVE.sand}`, fontSize: SIZES.base,
              background: HIVE.cream,
            }}
          />
        </label>

        <label style={{ display: 'block', marginBottom: 16 }}>
          <div style={{ fontSize: SIZES.sm, color: HIVE.dimText, marginBottom: 4 }}>
            Colony ID (auto-detected if empty)
          </div>
          <input
            type="text"
            value={colonyId}
            onChange={e => setColonyId(e.target.value)}
            placeholder="Auto-detect from VPS"
            style={{
              width: '100%', padding: '8px 12px', borderRadius: 8,
              border: `1px solid ${HIVE.sand}`, fontSize: SIZES.base,
              background: HIVE.cream,
            }}
          />
        </label>

        {status === 'error' && (
          <div style={{ fontSize: SIZES.sm, color: HIVE.alert, marginBottom: 12 }}>
            {errorMsg}
          </div>
        )}

        <div style={{ display: 'flex', gap: 8 }}>
          <button
            onClick={onClose}
            style={{
              flex: 1, padding: '10px 0', borderRadius: 12,
              border: `1px solid ${HIVE.sand}`, background: 'transparent',
              color: HIVE.soil, fontSize: SIZES.base, cursor: 'pointer',
            }}
          >
            Cancel
          </button>
          <button
            onClick={handleConnect}
            disabled={status === 'testing'}
            style={{
              flex: 1, padding: '10px 0', borderRadius: 12,
              border: 'none', background: HIVE.accent,
              color: HIVE.white, fontSize: SIZES.base, fontWeight: 600,
              cursor: 'pointer', opacity: status === 'testing' ? 0.6 : 1,
            }}
          >
            {status === 'testing' ? 'Connecting...' : 'Connect'}
          </button>
        </div>
      </div>
    </div>
  );
}
