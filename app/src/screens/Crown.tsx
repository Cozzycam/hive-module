/* Crown — the coronation ceremony (Gateway Phase 2).
 *
 * A fully-grown princess leaves the phone to found a colony on a physical
 * Hive Module: pick a freshly-plugged-in module, park her on the VPS under a
 * one-time claim token, queue summon_queen to the module, and watch until its
 * snapshot comes back wearing her name. Only then does she leave the phone
 * (local colony wiped, app connects to her new colony). Every failure path
 * leaves her safely where she is.
 */
import { useCallback, useEffect, useState, type ReactNode } from 'react';
import { localModule, resetColonyIdentity, WIPE_KEY } from '../localModule';
import { fetchColonies, fetchSnapshot, sendCommand, setStoredColonyId } from '../api/client';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';

// A module is crownable while its colony is YOUNG — the firmware refuses
// summon_queen on established colonies (2h window / founder-cohort gate).
const YOUNG_WINDOW_S = 2 * 3600;
const SUMMON_FW_MIN = 217;

interface Candidate {
  colony_id: string;
  queen_name: string;
  founded_unix: number;
  fw_version: number;
  young: boolean;
  fwOk: boolean;
}

type Phase = 'pick' | 'confirm' | 'working' | 'done' | 'failed';

export function Crown({ onBack }: { onBack: () => void }) {
  const [candidates, setCandidates] = useState<Candidate[] | null>(null);
  const [chosen, setChosen] = useState<Candidate | null>(null);
  const [phase, setPhase] = useState<Phase>('pick');
  const [progress, setProgress] = useState('');
  const [error, setError] = useState('');

  const princess = localModule.princessStats();
  const exported = localModule.exportPrincess();
  const name = String(exported?.name ?? 'your princess');
  const ready = (princess?.maturity ?? 0) >= 1;

  // Scan for crownable modules: hardware colonies (non app-*), youngest first.
  const scan = useCallback(async () => {
    setCandidates(null);
    const list = await fetchColonies();
    if (!list) { setCandidates([]); return; }
    const hw = list.colonies.filter(c => !c.colony_id.startsWith('app-'));
    const out: Candidate[] = [];
    for (const c of hw.slice(0, 12)) {
      const snap = await fetchSnapshot(c.colony_id);
      if (!snap) continue;
      const s = snap.data;
      const now = Math.floor(Date.now() / 1000);
      out.push({
        colony_id: c.colony_id,
        queen_name: s.queen_name || '(unfounded)',
        founded_unix: s.founded_unix || 0,
        fw_version: s.fw_version || 0,
        young: now - (s.founded_unix || 0) < YOUNG_WINDOW_S,
        fwOk: (s.fw_version || 0) >= SUMMON_FW_MIN,
      });
    }
    out.sort((a, b) => b.founded_unix - a.founded_unix);
    setCandidates(out);
  }, []);

  useEffect(() => { void scan(); }, [scan]);

  const crown = useCallback(async (target: Candidate) => {
    setPhase('working');
    setError('');
    try {
      setProgress('Preparing her for the journey…');
      const token = await localModule.parkHandoff();
      if (!token) throw new Error('Could not park her on the hive — check your connection and try again.');

      setProgress('Calling the module…');
      const queued = await sendCommand(target.colony_id, 'summon_queen', { token });
      if (!queued) throw new Error('The module could not be reached — she is still safely here.');

      // The module polls every ~30s, then wipes + reboots + refounds (~1 min).
      setProgress('Waiting for the module to crown her… (this takes a minute or two)');
      const deadline = Date.now() + 4 * 60 * 1000;
      while (Date.now() < deadline) {
        await new Promise(r => setTimeout(r, 10000));
        const snap = await fetchSnapshot(target.colony_id);
        if (snap && snap.data.queen_name === name && snap.source !== 'cache') {
          // She reigns. Only NOW does she leave the phone: wipe the local
          // colony, mint a fresh install identity for a future egg, and
          // follow her to the module.
          setProgress('She has taken the throne 👑');
          resetColonyIdentity();
          localStorage.setItem(WIPE_KEY, '1');
          setStoredColonyId(target.colony_id);
          setPhase('done');
          setTimeout(() => window.location.reload(), 3500);
          return;
        }
      }
      throw new Error('The module hasn’t answered yet — she is still safely here. Check it’s plugged in and online, then try again.');
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Something went wrong — she is still safely here.');
      setPhase('failed');
    }
  }, [name]);

  const card = (body: ReactNode) => (
    <div style={{ width: '100%', maxWidth: 340, borderRadius: 14, padding: '16px 18px',
                  background: HIVE.cream, border: `1px solid ${HIVE.parchment}` }}>
      {body}
    </div>
  );

  return (
    <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 14, padding: '14px 16px 24px' }}>
      <div style={{ width: '100%', maxWidth: 340, display: 'flex', alignItems: 'center', gap: 8 }}>
        {phase !== 'working' && phase !== 'done' && (
          <button onClick={onBack} aria-label="Back"
            style={{ background: 'none', border: 'none', cursor: 'pointer', fontSize: 20, color: HIVE.soil }}>
            ‹
          </button>
        )}
        <span style={{ fontSize: SIZES.base, fontWeight: 700, color: HIVE.ink }}>👑 Coronation</span>
      </div>

      {!ready && phase === 'pick' && card(
        <span style={{ fontSize: SIZES.sm, color: HIVE.soil }}>
          {name} isn’t fully grown yet — she can be crowned once she’s ready to bloom.
        </span>
      )}

      {ready && phase === 'pick' && (
        <>
          {card(
            <span style={{ fontSize: SIZES.sm, color: HIVE.soil }}>
              When {name} moves to a Hive Module she’ll be crowned its queen and found a colony
              of her own — same name, same colours, the personality you raised. Her colony will
              take after her.<br /><br />
              Plug in a <b>new</b> Hive Module and it will appear below within a minute of coming online.
            </span>
          )}
          {candidates === null && <span style={{ fontSize: SIZES.sm, color: HIVE.dimText }}>looking for modules…</span>}
          {candidates !== null && candidates.length === 0 &&
            <span style={{ fontSize: SIZES.sm, color: HIVE.dimText }}>No modules found yet.</span>}
          {candidates?.map(c => {
            const crownable = c.young && c.fwOk;
            const why = !c.fwOk ? 'needs a firmware update first'
                      : !c.young ? 'already an established colony'
                      : 'ready for a queen';
            return (
              <button key={c.colony_id} disabled={!crownable}
                onClick={() => { setChosen(c); setPhase('confirm'); }}
                style={{ width: '100%', maxWidth: 340, textAlign: 'left', borderRadius: 12,
                         padding: '12px 14px', cursor: crownable ? 'pointer' : 'default',
                         border: `1px solid ${crownable ? HIVE.green : HIVE.parchment}`,
                         background: HIVE.white, opacity: crownable ? 1 : 0.55 }}>
                <div style={{ fontSize: SIZES.sm, fontWeight: 600, color: HIVE.ink }}>{c.colony_id}</div>
                <div style={{ fontSize: SIZES.xs, color: HIVE.dimText }}>
                  queen {c.queen_name} · fw v{c.fw_version} · {why}
                </div>
              </button>
            );
          })}
          <button onClick={() => void scan()}
            style={{ fontSize: SIZES.xs, padding: '6px 12px', borderRadius: 8, cursor: 'pointer',
                     border: `1px solid ${HIVE.sand}`, background: HIVE.parchment, color: HIVE.soil }}>
            ↻ Look again
          </button>
        </>
      )}

      {phase === 'confirm' && chosen && card(
        <>
          <div style={{ fontSize: SIZES.sm, color: HIVE.soil }}>
            Crown <b>{name}</b> onto <b>{chosen.colony_id}</b>?<br /><br />
            The module’s just-founded colony (queen {chosen.queen_name}) will make way for her.
            She’ll leave the phone for good — the nest here will be empty until you hatch a new egg.
          </div>
          <div style={{ display: 'flex', gap: 8, marginTop: 12 }}>
            <button onClick={() => void crown(chosen)}
              style={{ flex: 1, padding: '10px', borderRadius: 10, border: 'none', cursor: 'pointer',
                       background: HIVE.green, color: HIVE.white, fontWeight: 600 }}>
              👑 Crown her
            </button>
            <button onClick={() => setPhase('pick')}
              style={{ flex: 1, padding: '10px', borderRadius: 10, cursor: 'pointer',
                       border: `1px solid ${HIVE.sand}`, background: HIVE.parchment, color: HIVE.soil }}>
              Not yet
            </button>
          </div>
        </>
      )}

      {phase === 'working' && card(
        <span style={{ fontSize: SIZES.sm, color: HIVE.soil }}>⏳ {progress}</span>
      )}

      {phase === 'done' && card(
        <span style={{ fontSize: SIZES.sm, color: HIVE.soil }}>
          👑 <b>Queen {name}</b> has founded her colony. Taking you to her new home…
        </span>
      )}

      {phase === 'failed' && card(
        <>
          <div style={{ fontSize: SIZES.sm, color: '#a04040' }}>{error}</div>
          <button onClick={() => setPhase('pick')}
            style={{ marginTop: 10, padding: '8px 14px', borderRadius: 8, cursor: 'pointer',
                     border: `1px solid ${HIVE.sand}`, background: HIVE.parchment, color: HIVE.soil }}>
            Back
          </button>
        </>
      )}
    </div>
  );
}
