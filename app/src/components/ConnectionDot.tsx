import { useState } from 'react';
import { useColony, timeSince } from '../state/colony';
import { CONNECTION_COLORS, HIVE } from '../theme/palette';
import type { DataSource } from '../api/types';

const SOURCE_LABELS: Record<DataSource, string> = {
  lan: 'Connected to queen (LAN)',
  vps: 'Connected via cloud',
  cache: 'Offline (cached data)',
  none: 'No connection',
};

export function ConnectionDot() {
  const { source, lastFetchMs } = useColony();
  const [expanded, setExpanded] = useState(false);
  const color = CONNECTION_COLORS[source];

  return (
    <div style={{ position: 'relative' }}>
      <button
        onClick={() => setExpanded(!expanded)}
        aria-label={SOURCE_LABELS[source]}
        style={{
          width: 10,
          height: 10,
          borderRadius: '50%',
          background: color,
          border: 'none',
          cursor: 'pointer',
          boxShadow: `0 0 4px ${color}`,
          padding: 0,
        }}
      />
      {expanded && (
        <div
          style={{
            position: 'absolute',
            top: 18,
            right: 0,
            background: HIVE.white,
            borderRadius: 12,
            padding: '10px 14px',
            boxShadow: '0 2px 12px rgba(0,0,0,0.12)',
            whiteSpace: 'nowrap',
            zIndex: 100,
            fontSize: 13,
            color: HIVE.ink,
          }}
        >
          <div style={{ fontWeight: 600, marginBottom: 4 }}>
            {SOURCE_LABELS[source]}
          </div>
          <div style={{ color: HIVE.dimText }}>
            Last updated: {timeSince(lastFetchMs)}
          </div>
        </div>
      )}
    </div>
  );
}
