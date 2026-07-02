// Trait metadata shared by Characters (badges), biography (life stories)
// and anywhere else a trait id needs humanizing.
// (Amber feedback: "make the traits be understandable")
export const TRAIT_INFO: Record<string, { label: string; desc: string }> = {
  pioneer: { label: 'Pioneer', desc: 'First to step into an unexplored chamber.' },
  elder: { label: 'Elder', desc: 'Has lived to a grand old age — the young ones huddle close.' },
  bonded: {
    label: 'Bonded',
    desc: 'Made a true friend by spending lots of time side by side. '
        + 'Friendships can fade if they drift apart — or be lost when a friend dies — '
        + 'but the badge is theirs to keep.',
  },
  survived_heatwave: { label: 'Heatwave Survivor', desc: 'Came through a scorching spell.' },
  survived_cold_snap: { label: 'Cold Snap Survivor', desc: 'Endured a bitter freeze.' },
  survived_drought: { label: 'Drought Survivor', desc: 'Outlasted the hungry, dry days.' },
  survived_storm: { label: 'Storm Survivor', desc: 'Weathered a great storm.' },
  catcher: { label: 'Bug Hunter', desc: "The colony's reigning critter-catcher — a title that only passes to whoever out-catches the current champion." },
};

export function traitLabel(t: string): string {
  return TRAIT_INFO[t]?.label ?? t.replace(/_/g, ' ');
}

export const SURVIVAL_TRAITS = [
  'survived_heatwave', 'survived_cold_snap', 'survived_drought', 'survived_storm',
];
