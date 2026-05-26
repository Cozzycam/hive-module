import { createContext, useContext } from 'react';
import type { ColonySnapshot, EventsResponse, DataSource, ColonyEvent } from '../api/types';

export interface ColonyState {
  snapshot: ColonySnapshot | null;
  events: ColonyEvent[];
  source: DataSource;
  lastFetchMs: number;
  colonyId: string | null;
  loading: boolean;
}

export interface ColonyActions {
  refreshEvents: (since?: number) => Promise<void>;
  setColonyId: (id: string) => void;
  disconnect: () => void;
}

export const ColonyContext = createContext<ColonyState>({
  snapshot: null,
  events: [],
  source: 'none',
  lastFetchMs: 0,
  colonyId: null,
  loading: true,
});

export const ColonyActionsContext = createContext<ColonyActions>({
  refreshEvents: async () => {},
  setColonyId: () => {},
  disconnect: () => {},
});

export function useColony(): ColonyState {
  return useContext(ColonyContext);
}

export function useColonyActions(): ColonyActions {
  return useContext(ColonyActionsContext);
}

// Helper: time since last fetch, human-readable
export function timeSince(ms: number): string {
  if (!ms) return 'never';
  const secs = Math.floor((Date.now() - ms) / 1000);
  if (secs < 5) return 'just now';
  if (secs < 60) return `${secs}s ago`;
  const mins = Math.floor(secs / 60);
  if (mins < 60) return `${mins}m ago`;
  const hours = Math.floor(mins / 60);
  return `${hours}h ago`;
}
