import type { CSSProperties } from 'react';
import { HIVE } from '../theme/palette';

interface ChipProps {
  label: string;
  active?: boolean;
  onClick?: () => void;
  style?: CSSProperties;
}

export function Chip({ label, active, onClick, style }: ChipProps) {
  return (
    <button
      onClick={onClick}
      style={{
        display: 'inline-block',
        padding: '4px 12px',
        borderRadius: 20,
        border: `1px solid ${active ? HIVE.accent : HIVE.sand}`,
        background: active ? HIVE.accent : 'transparent',
        color: active ? HIVE.white : HIVE.soil,
        fontSize: 13,
        fontWeight: 500,
        cursor: onClick ? 'pointer' : 'default',
        whiteSpace: 'nowrap',
        ...style,
      }}
    >
      {label}
    </button>
  );
}
