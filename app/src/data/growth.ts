// Mirror of the firmware's render_scale() (conker.h): hatchlings render at
// 60% size and grow to full over their first day. The app applies the same
// curve so the nest view and profile match what the glass shows —
// a newborn reads as a bub everywhere, not "1 cm" of adult.
export function growthFactor(ageDays?: number): number {
  if (ageDays === undefined || ageDays >= 1) return 1;
  return 0.6 + 0.4 * Math.max(0, ageDays);
}
