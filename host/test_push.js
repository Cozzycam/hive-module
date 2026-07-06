// Validate the phone module's enrol + push against a LOCAL VPS (node vps/server.js).
// Proves the TOFU/HMAC contract end-to-end without touching production.
// Usage: node host/test_push.js  (with the local server running on :3999)
const crypto = require('crypto');

const BASE = process.env.VPS_BASE || 'http://localhost:3999';
const cid = 'phonetest-' + crypto.randomBytes(5).toString('hex');
const secret = crypto.randomBytes(24).toString('hex');   // 48 hex chars (in [32,128])

// A representative snapshot body (shape matches api_colony_json; content is
// irrelevant to the push mechanics — the server stores the raw bytes).
const snapshot = {
  schema: 1, colony_id: cid, queen_name: 'Balsam', fw_version: 214,
  founded_unix: 1751000000, now_unix: Math.floor(1751000000 + 7200),
  population: { alive: 3, dead_total: 0, cap: 10, eggs_dormant: false },
  grid: { w: 30, h: 20 }, queen_pos: { x: 15, y: 10 }, lilguys: [], food: {}, modules: [],
};
const body = Buffer.from(JSON.stringify(snapshot), 'utf8');
const sign = (sec) => crypto.createHmac('sha256', sec).update(body).digest('hex');

async function post(headers) {
  const r = await fetch(`${BASE}/api/v1/colonies/${cid}/snapshot`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', ...headers },
    body,
  });
  return { status: r.status, text: await r.text() };
}

(async () => {
  console.log('colony_id:', cid);
  console.log('secret len:', secret.length);

  // 1. First contact WITHOUT enrol header → must be rejected (401).
  const noEnroll = await post({ 'x-hmac-sha256': sign(secret) });
  console.log('1) push w/o enrol header  ->', noEnroll.status, '(expect 401)');

  // 2. First contact WITH enrol header + correct signature → TOFU enrol + accept.
  const enrol = await post({ 'x-enroll-secret': secret, 'x-hmac-sha256': sign(secret) });
  console.log('2) enrol push             ->', enrol.status, '(expect 200/2xx)');

  // 3. Subsequent push signed with the per-colony secret (no enrol header) → accepted.
  const resend = await post({ 'x-hmac-sha256': sign(secret) });
  console.log('3) authed push (enrolled) ->', resend.status, '(expect 2xx)');

  // 4. Push with a WRONG signature → rejected.
  const bad = await post({ 'x-hmac-sha256': 'deadbeef'.repeat(8) });
  console.log('4) push w/ bad signature  ->', bad.status, '(expect 401)');

  // 5. The colony is now readable via the public GET the app uses.
  const got = await fetch(`${BASE}/api/v1/colonies/${cid}`);
  const gotBody = got.ok ? JSON.parse(await got.text()) : null;
  console.log('5) GET /colonies/:id      ->', got.status,
              gotBody ? `(queen=${gotBody.queen_name}, alive=${gotBody.population?.alive})` : '');

  const pass = noEnroll.status === 401 && enrol.status < 300 && resend.status < 300
            && bad.status === 401 && got.status === 200;
  console.log(pass ? '\nPASS: enrol + push + read cycle works.' : '\nFAIL: see statuses above.');
})();
