import type { ReactNode } from 'react';
import { HIVE } from '../theme/palette';
import { artForm, artKindEmoji } from '../data/artworks';

// Small SVG of the actual form a work took — hand-drawn approximations of
// the on-device sprites, so a gallery card shows what stands in the
// chamber instead of a generic emoji. Accessories and unknown kinds keep
// their emoji. On the device a work renders in its maker's own hue, but
// events don't carry the tint — warm clay stands in for every maker.
const MAIN = '#C99A5C';
const DARK = HIVE.bark;
const LITE = HIVE.cream;
const STONE = '#A29A8E';
const STONE_DARK = '#6E675C';

export function ArtGlyph({ kind, motif, size = 34 }: {
  kind: string;
  motif?: number;
  size?: number;
}) {
  const form = artForm(kind, motif);
  if (form === null) {
    return <span style={{ fontSize: size * 0.8, lineHeight: 1 }}>{artKindEmoji(kind)}</span>;
  }

  const svg = (children: ReactNode) => (
    <svg width={size} height={size} viewBox="0 0 24 24" aria-hidden="true">
      {children}
    </svg>
  );

  switch (form) {
    case 'orb':
      return svg(<>
        <rect x="7" y="19" width="10" height="2.4" rx="1" fill={DARK} />
        <rect x="10.4" y="15.5" width="3.2" height="4" fill={DARK} />
        <circle cx="12" cy="9.5" r="6.2" fill={MAIN} stroke={DARK} strokeWidth="1" />
        <circle cx="9.8" cy="7.2" r="1.6" fill={LITE} opacity="0.8" />
      </>);
    case 'spire':
      return svg(<>
        <rect x="7" y="19" width="10" height="2.4" rx="1" fill={DARK} />
        <path d="M12 2.5 L15.5 19 L8.5 19 Z" fill={MAIN}
              stroke={DARK} strokeWidth="1" strokeLinejoin="round" />
        <path d="M11.2 6 L9.9 18" stroke={LITE} strokeWidth="1"
              fill="none" strokeLinecap="round" opacity="0.55" />
      </>);
    case 'arch':
      return svg(
        <path d="M5.5 21 V10.5 Q5.5 4.5 12 4.5 Q18.5 4.5 18.5 10.5 V21 H14.6
                 V11.2 Q14.6 8.6 12 8.6 Q9.4 8.6 9.4 11.2 V21 Z"
              fill={MAIN} stroke={DARK} strokeWidth="1" strokeLinejoin="round" />
      );
    case 'cairn':
      return svg(<>
        <ellipse cx="12" cy="18.4" rx="7" ry="3.1" fill={MAIN} stroke={DARK} strokeWidth="1" />
        <ellipse cx="12.6" cy="13.6" rx="5.2" ry="2.8" fill={MAIN} stroke={DARK} strokeWidth="1" />
        <ellipse cx="11.4" cy="9.6" rx="3.6" ry="2.4" fill={MAIN} stroke={DARK} strokeWidth="1" />
      </>);
    case 'paint_diamonds':
      return svg(<>
        <rect x="3.5" y="3.5" width="17" height="17" rx="3"
              fill={LITE} stroke={DARK} strokeWidth="1" />
        <path d="M12 8.6 L14.4 12 L12 15.4 L9.6 12 Z" fill={MAIN} />
        <path d="M7 5.2 L8.6 7.4 L7 9.6 L5.4 7.4 Z" fill={MAIN} opacity="0.75" />
        <path d="M17 5.2 L18.6 7.4 L17 9.6 L15.4 7.4 Z" fill={MAIN} opacity="0.75" />
        <path d="M7 14.4 L8.6 16.6 L7 18.8 L5.4 16.6 Z" fill={MAIN} opacity="0.75" />
        <path d="M17 14.4 L18.6 16.6 L17 18.8 L15.4 16.6 Z" fill={MAIN} opacity="0.75" />
      </>);
    case 'paint_meander':
      return svg(<>
        <rect x="3.5" y="3.5" width="17" height="17" rx="3"
              fill={LITE} stroke={DARK} strokeWidth="1" />
        <path d="M6 18 C9 13, 15 11, 18 6" stroke={MAIN} strokeWidth="1.8"
              fill="none" strokeLinecap="round" />
        <path d="M6 6 C9 11, 15 13, 18 18" stroke={MAIN} strokeWidth="1.8"
              fill="none" strokeLinecap="round" opacity="0.75" />
      </>);
    case 'memorial':
      return svg(<>
        <rect x="5.5" y="19" width="13" height="2.4" rx="1" fill={STONE_DARK} />
        <path d="M7 19.4 V9.5 Q7 4 12 4 Q17 4 17 9.5 V19.4 Z"
              fill={STONE} stroke={STONE_DARK} strokeWidth="1" />
        <rect x="9.4" y="9.4" width="5.2" height="1.3" rx="0.6" fill={STONE_DARK} opacity="0.75" />
        <rect x="9.4" y="12.4" width="5.2" height="1.3" rx="0.6" fill={STONE_DARK} opacity="0.75" />
      </>);
    case 'lost':
      // The event predates the motif field — a soft outline where it stood
      return svg(<>
        <ellipse cx="12" cy="13" rx="6.5" ry="5.5" fill={MAIN} opacity="0.22" />
        <ellipse cx="12" cy="13" rx="6.5" ry="5.5" fill="none" stroke={DARK}
                 strokeWidth="1" strokeDasharray="2.6 2.6" opacity="0.7" />
      </>);
  }
}
