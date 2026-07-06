/* hivePush — enrol this install's own colony and push its snapshots to the VPS,
 * exactly like a hardware queen module (module → VPS → app).
 *
 * Each install owns ONE colony: a colony_id + secret generated once and kept in
 * localStorage. First push TOFU-enrols (x-enroll-secret + HMAC); later pushes
 * just sign with the per-colony secret. Signing is WebCrypto HMAC-SHA256 over
 * the exact request-body bytes — verified to match the server's node crypto.
 *
 * In production the app and API are the same origin (hive.campbell.fish), so
 * base '' works and there's no CORS. Pass an absolute base only for testing.
 */
(function (global) {
  const ID_KEY = 'hive_module_identity_v1';   // { colony_id, secret, enrolled }

  function newSecret() {
    const b = new Uint8Array(24);              // 48 hex chars (in the 32–128 range)
    crypto.getRandomValues(b);
    return [...b].map(x => x.toString(16).padStart(2, '0')).join('');
  }
  function newColonyId() {
    const b = new Uint8Array(6);
    crypto.getRandomValues(b);
    return 'app-' + [...b].map(x => x.toString(16).padStart(2, '0')).join('');
  }

  function getIdentity() {
    try {
      const raw = localStorage.getItem(ID_KEY);
      if (raw) return JSON.parse(raw);
    } catch { /* fall through */ }
    const id = { colony_id: newColonyId(), secret: newSecret(), enrolled: false };
    localStorage.setItem(ID_KEY, JSON.stringify(id));
    return id;
  }
  function saveIdentity(id) { localStorage.setItem(ID_KEY, JSON.stringify(id)); }

  async function hmacHex(secret, bodyStr) {
    const enc = new TextEncoder();
    const key = await crypto.subtle.importKey(
      'raw', enc.encode(secret), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const sig = await crypto.subtle.sign('HMAC', key, enc.encode(bodyStr));
    return [...new Uint8Array(sig)].map(b => b.toString(16).padStart(2, '0')).join('');
  }

  // snapshotJson: the raw string from host_snapshot(). We stamp our colony_id
  // into it (a fresh no-SD colony emits an empty one).
  async function pushSnapshot(base, snapshotJson) {
    const id = getIdentity();
    const body = snapshotJson.replace('"colony_id":""', `"colony_id":"${id.colony_id}"`);
    const sig = await hmacHex(id.secret, body);
    const headers = { 'Content-Type': 'application/json', 'x-hmac-sha256': sig };
    if (!id.enrolled) headers['x-enroll-secret'] = id.secret;
    const res = await fetch(`${base}/api/v1/colonies/${id.colony_id}/snapshot`, {
      method: 'POST', headers, body,
    });
    if (res.ok && !id.enrolled) { id.enrolled = true; saveIdentity(id); }
    return { status: res.status, colony_id: id.colony_id };
  }

  // Drive a running module: pull host_snapshot() and push every `intervalMs`
  // (hardware pushes every 30s). Returns a stop() function.
  function startPushing({ base = '', intervalMs = 30000, getSnapshotJson, onResult }) {
    let stopped = false;
    async function tick() {
      if (stopped) return;
      try {
        const r = await pushSnapshot(base, getSnapshotJson());
        onResult && onResult(r);
      } catch (e) { onResult && onResult({ error: String(e) }); }
      if (!stopped) setTimeout(tick, intervalMs);
    }
    tick();
    return () => { stopped = true; };
  }

  global.hivePush = { getIdentity, pushSnapshot, startPushing };
})(window);
