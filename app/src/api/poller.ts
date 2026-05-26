import { fetchSnapshot, fetchEvents, type FetchResult } from './client';
import type { ColonySnapshot, EventsResponse, DataSource } from './types';

export interface PollerState {
  snapshot: ColonySnapshot | null;
  events: EventsResponse | null;
  source: DataSource;
  lastFetchMs: number;
  error: string | null;
}

type Listener = (state: PollerState) => void;

export class Poller {
  private colonyId: string;
  private interval: number;
  private timer: ReturnType<typeof setInterval> | null = null;
  private listeners = new Set<Listener>();
  private state: PollerState = {
    snapshot: null,
    events: null,
    source: 'none',
    lastFetchMs: 0,
    error: null,
  };
  private visibilityHandler: (() => void) | null = null;

  constructor(colonyId: string, intervalMs = 15_000) {
    this.colonyId = colonyId;
    this.interval = intervalMs;
  }

  subscribe(fn: Listener): () => void {
    this.listeners.add(fn);
    fn(this.state);
    return () => this.listeners.delete(fn);
  }

  private notify() {
    for (const fn of this.listeners) fn(this.state);
  }

  async poll(): Promise<void> {
    const result = await fetchSnapshot(this.colonyId);
    if (result) {
      this.state = {
        ...this.state,
        snapshot: result.data,
        source: result.source,
        lastFetchMs: result.timestamp,
        error: null,
      };
    } else if (!this.state.snapshot) {
      this.state = { ...this.state, source: 'none', error: 'unreachable' };
    }
    this.notify();
  }

  async pollEvents(since = 0): Promise<void> {
    const result = await fetchEvents(this.colonyId, since);
    if (result) {
      this.state = { ...this.state, events: result.data };
      this.notify();
    }
  }

  start(): void {
    this.poll();
    this.timer = setInterval(() => this.poll(), this.interval);

    this.visibilityHandler = () => {
      if (document.visibilityState === 'hidden') {
        this.stop(true);
      } else {
        this.poll();
        this.timer = setInterval(() => this.poll(), this.interval);
      }
    };
    document.addEventListener('visibilitychange', this.visibilityHandler);
  }

  stop(keepVisibility = false): void {
    if (this.timer) {
      clearInterval(this.timer);
      this.timer = null;
    }
    if (!keepVisibility && this.visibilityHandler) {
      document.removeEventListener('visibilitychange', this.visibilityHandler);
      this.visibilityHandler = null;
    }
  }

  setColonyId(id: string): void {
    this.colonyId = id;
    this.poll();
  }

  getState(): PollerState {
    return this.state;
  }
}
