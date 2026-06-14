import { useState } from 'react';
import { useColony } from '../state/colony';
import { useTOD } from '../state/tod';
import { Card } from '../components/Card';
import { sendFeedback } from '../api/client';
import { HIVE } from '../theme/palette';
import { SIZES } from '../theme/fonts';

const APP_VERSION = '1.5.1';

export function Feedback() {
  const { snapshot, colonyId, source } = useColony();
  const tod = useTOD(true);
  const [text, setText] = useState('');
  const [state, setState] = useState<'idle' | 'sending' | 'sent' | 'failed'>('idle');

  const submit = async () => {
    const trimmed = text.trim();
    if (!trimmed || state === 'sending') return;
    setState('sending');
    const ok = await sendFeedback(colonyId, trimmed, {
      fw_version: snapshot?.fw_version ?? null,
      app_version: APP_VERSION,
      source,
      ua: navigator.userAgent.slice(0, 120),
    });
    if (ok) {
      setState('sent');
      setText('');
    } else {
      setState('failed');
    }
  };

  return (
    <div style={{ background: tod.bg, minHeight: '100%', padding: '0 16px 100px' }}>
      <h1 style={{ fontSize: SIZES.xl, fontWeight: 700, color: tod.text, padding: '16px 0 4px' }}>
        Feedback
      </h1>
      <div style={{ fontSize: SIZES.sm, color: tod.dimText, marginBottom: 16 }}>
        Spotted something odd? Wish the conkers did something? Tell us —
        it goes straight to the workshop.
      </div>

      <Card style={{ background: tod.cardBg }}>
        <textarea
          value={text}
          onChange={e => { setText(e.target.value); if (state === 'sent' || state === 'failed') setState('idle'); }}
          placeholder="What happened? What would make it better?"
          rows={6}
          maxLength={2000}
          style={{
            width: '100%', boxSizing: 'border-box', resize: 'vertical',
            background: 'transparent', color: tod.text,
            border: `1px solid ${HIVE.sand}`, borderRadius: 8,
            padding: 10, fontSize: SIZES.base, fontFamily: 'inherit',
          }}
        />
        <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginTop: 10 }}>
          <span style={{ fontSize: SIZES.xs, color: tod.dimText }}>
            {state === 'sent' ? 'Sent — thank you! 🌰'
              : state === 'failed' ? 'Couldn’t send — try again in a moment.'
              : `${text.length}/2000`}
          </span>
          <button
            onClick={submit}
            disabled={!text.trim() || state === 'sending'}
            style={{
              background: text.trim() ? HIVE.accent : HIVE.sand,
              color: HIVE.white, border: 'none', borderRadius: 18,
              padding: '8px 20px', fontSize: SIZES.sm, fontWeight: 600,
              cursor: text.trim() ? 'pointer' : 'default',
            }}
          >
            {state === 'sending' ? 'Sending…' : 'Send'}
          </button>
        </div>
      </Card>

      <div style={{ fontSize: SIZES.xs, color: tod.dimText, marginTop: 12, lineHeight: 1.5 }}>
        Sent with your note: colony {colonyId || '—'}, firmware v{snapshot?.fw_version ?? '?'},
        app v{APP_VERSION}. No account, no personal data.
      </div>
    </div>
  );
}
