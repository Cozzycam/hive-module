import { createContext, useContext, useState, useCallback, type ReactNode } from 'react';
import React from 'react';

const PINS_KEY = 'hive_pins';

interface PinsState {
  pins: number[];
  isPinned: (id: number) => boolean;
  togglePin: (id: number) => void;
  addPin: (id: number) => void;
  removePin: (id: number) => void;
}

const PinsContext = createContext<PinsState>({
  pins: [],
  isPinned: () => false,
  togglePin: () => {},
  addPin: () => {},
  removePin: () => {},
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

export function PinsProvider({ children }: { children: ReactNode }) {
  const [pins, setPins] = useState<number[]>(loadPins);

  const isPinned = useCallback((id: number) => pins.includes(id), [pins]);

  const addPin = useCallback((id: number) => {
    setPins(prev => {
      if (prev.includes(id)) return prev;
      const next = [...prev, id];
      savePins(next);
      return next;
    });
  }, []);

  const removePin = useCallback((id: number) => {
    setPins(prev => {
      const next = prev.filter(p => p !== id);
      savePins(next);
      return next;
    });
  }, []);

  const togglePin = useCallback((id: number) => {
    setPins(prev => {
      const next = prev.includes(id) ? prev.filter(p => p !== id) : [...prev, id];
      savePins(next);
      return next;
    });
  }, []);

  return React.createElement(PinsContext.Provider, {
    value: { pins, isPinned, togglePin, addPin, removePin },
    children,
  });
}

export function usePins(): PinsState {
  return useContext(PinsContext);
}
