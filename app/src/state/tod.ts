import { useMemo } from 'react';
import { useColony } from './colony';
import { interpolatePalette, TOD_PALETTES, type TodPalette } from '../theme/palette';

export function useTOD(liveMode = true): TodPalette {
  const { snapshot } = useColony();

  return useMemo(() => {
    if (!liveMode || !snapshot) return TOD_PALETTES.day;
    const { phase, night_factor } = snapshot.world.tod;
    return interpolatePalette(phase, night_factor);
  }, [liveMode, snapshot?.world?.tod?.phase, snapshot?.world?.tod?.night_factor]);
}
