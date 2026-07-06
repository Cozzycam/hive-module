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

  const onPointer = (e: React.PointerEvent) => {
    e.preventDefault();
    if (status === 'running') localModule.feedAt(e.clientX, e.clientY);
  };

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8, padding: '8px 12px' }}>
      <div
        ref={holderRef}
        onPointerDown={onPointer}
        style={{ width: '100%', maxWidth: 'calc((100vh - 220px) * 1.5)', minHeight: 60 }}
      />
      <div style={{ fontSize: 12, opacity: 0.55, fontFamily: 'ui-monospace, Consolas, monospace' }}>
        {status === 'loading' && 'warming up your colony…'}
        {status === 'error'   && 'could not start the module'}
        {status === 'running' && 'your colony · tap the glass to feed · syncing to the hive'}
      </div>
    </div>
  );
}
