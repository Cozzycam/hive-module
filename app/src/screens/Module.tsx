/* Module — displays this install's live queen module.
 *
 * The module itself runs in the localModule singleton (so it keeps ticking +
 * pushing to the VPS regardless of which tab is open); this screen just mounts
 * its canvas and forwards taps. The rest of the app reads the colony back from
 * the VPS like any hardware colony.
 */
import { useEffect, useRef, useState } from 'react';
import { localModule } from '../localModule';

export default function Module() {
  const holderRef = useRef<HTMLDivElement | null>(null);
  const [status, setStatus] = useState<'loading' | 'running' | 'error'>('loading');

  useEffect(() => {
    let cancelled = false;
    localModule.start()
      .then(() => {
        if (cancelled) return;
        const holder = holderRef.current;
        if (holder && !holder.contains(localModule.canvas)) {
          localModule.canvas.style.width = '100%';
          localModule.canvas.style.height = 'auto';
          localModule.canvas.style.aspectRatio = '480 / 320';
          localModule.canvas.style.borderRadius = '10px';
          localModule.canvas.style.touchAction = 'none';
          localModule.canvas.style.background = '#0b0d0a';
          holder.appendChild(localModule.canvas);
        }
        setStatus('running');
      })
      .catch((e) => { console.error('[module]', e); if (!cancelled) setStatus('error'); });
    return () => { cancelled = true; };  // leave the module running; just detach view
  }, []);

  // Gesture: a quick tap boops/feeds; press-and-hold gathers the colony to
  // your finger (like the physical module's touch).
  const gesture = useRef<{ x: number; y: number; gathering: boolean; timer: number } | null>(null);
  const HOLD_MS = 200, MOVE_PX = 14;

  const onDown = (e: React.PointerEvent) => {
    if (status !== 'running') return;
    (e.target as Element).setPointerCapture?.(e.pointerId);
    const g = { x: e.clientX, y: e.clientY, gathering: false, timer: 0 };
    g.timer = window.setTimeout(() => { g.gathering = true; localModule.gatherAt(e.clientX, e.clientY); }, HOLD_MS);
    gesture.current = g;
  };
  const onMove = (e: React.PointerEvent) => {
    const g = gesture.current; if (!g) return;
    if (!g.gathering && Math.hypot(e.clientX - g.x, e.clientY - g.y) > MOVE_PX) {
      clearTimeout(g.timer); g.gathering = true;
    }
    if (g.gathering) localModule.gatherAt(e.clientX, e.clientY);
  };
  const onUp = (e: React.PointerEvent) => {
    const g = gesture.current; if (!g) return;
    clearTimeout(g.timer);
    if (g.gathering) localModule.gatherEnd();
    else localModule.tapAt(e.clientX, e.clientY);
    gesture.current = null;
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8, padding: '8px 12px' }}>
      <div
        ref={holderRef}
        onPointerDown={onDown}
        onPointerMove={onMove}
        onPointerUp={onUp}
        onPointerCancel={onUp}
        style={{ width: '100%', maxWidth: 'calc((100vh - 220px) * 1.5)', minHeight: 60, touchAction: 'none' }}
      />
      <div style={{ fontSize: 12, opacity: 0.55, fontFamily: 'ui-monospace, Consolas, monospace' }}>
        {status === 'loading' && 'warming up your colony…'}
        {status === 'error'   && 'could not start the module'}
        {status === 'running' && 'tap to boop or feed · hold to gather · syncing to the hive'}
      </div>
    </div>
  );
}
