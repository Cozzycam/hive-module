import type { CSSProperties, ReactNode } from 'react';

interface CardProps {
  children: ReactNode;
  style?: CSSProperties;
  onClick?: () => void;
}

export function Card({ children, style, onClick }: CardProps) {
  return (
    <div
      onClick={onClick}
      style={{
        background: '#F5ECDD',
        borderRadius: 16,
        padding: 16,
        marginBottom: 12,
        cursor: onClick ? 'pointer' : undefined,
        transition: 'transform 0.1s',
        ...style,
      }}
    >
      {children}
    </div>
  );
}
