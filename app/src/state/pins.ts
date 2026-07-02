import { createContext, useContext, useState, useCallback, type ReactNode } from 'react';
import React from 'react';

const PINS_KEY = 'hive_pins';
const PIN_NAMES_KEY = 'hive_pin_names';

interface PinsState {
  pins: number[];
  isPinned: (id: number) => boolean;
  togglePin: (id: number, name?: string) => void;
  addPin: (id: number, name?: string) => void;
  removePin: (id: number) => void;
  // Last-known display name for a pinned conker. The roster forgets the
  // dead (and renames), so the pin remembers what you followed them as —
  // fixes "I set lil baby to follow and it showed up as almond".
  pinName: (id: number) => string | null;
  rememberName: (id: number, name: string) => void;
}

const PinsContext = createContext<PinsState>({
  pins: [],
  isPinned: () => false,
  togglePin: () => {},
  addPin: () => {},
  removePin: () => {},
  pinName: () => null,
  rememberName: () => {},
});

function loadPins(): number[] {
  try {
    const raw = localStorage.getItem(PINS_KEY);
    if (!raw) return [];
    return JSON.parse(raw);
  } catch { return []; }
}

function savePins(pins: number[]): void {
  try { localStorage.setItem(PINS_KEY, JSON.stringify(pins)); } catch { /* */ }
}

function loadPinNames(): Record<number, string> {
  try {
    const raw = localStorage.getItem(PIN_NAMES_KEY);
    if (!raw) return {};
    return JSON.parse(raw);
  } catch { return {}; }
}

function savePinNames(names: Record<number, string>): void {
  try { localStorage.setItem(PIN_NAMES_KEY, JSON.stringify(names)); } catch { /* */ }
}

export function PinsProvider({ children }: { children: ReactNode }) {
  const [pins, setPins] = useState<number[]>(loadPins);
  const [names, setNames] = useState<Record<number, string>>(loadPinNames);

  const isPinned = useCallback((id: number) => pins.includes(id), [pins]);

  const rememberName = useCallback((id: number, name: string) => {
    if (!name) return;
    setNames(prev => {
      if (prev[id] === name) return prev;
      const next = { ...prev, [id]: name };
      savePinNames(next);
      return next;
    });
  }, []);

  const addPin = useCallback((id: number, name?: string) => {
    if (name) rememberName(id, name);
    setPins(prev => {
      if (prev.includes(id)) return prev;
      const next = [...prev, id];
      savePins(next);
      return next;
    });
  }, [rememberName]);

  const removePin = useCallback((id: number) => {
    setPins(prev => {
      const next = prev.filter(p => p !== id);
      savePins(next);
      return next;
    });
  }, []);

  const togglePin = useCallback((id: number, name?: string) => {
    if (name) rememberName(id, name);
    setPins(prev => {
      const next = prev.includes(id) ? prev.filter(p => p !== id) : [...prev, id];
      savePins(next);
      return next;
    });
  }, [rememberName]);

  const pinName = useCallback((id: number) => names[id] ?? null, [names]);

  return React.createElement(PinsContext.Provider, {
    value: { pins, isPinned, togglePin, addPin, removePin, pinName, rememberName },
    children,
  });
}

export function usePins(): PinsState {
  return useContext(PinsContext);
}
