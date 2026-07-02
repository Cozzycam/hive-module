/**
 * Hive Colony VPS Server
 * Receives snapshots + events from the queen via HTTPS POST.
 * Serves the same API shape back to the companion app.
 * SQLite for storage. HMAC-SHA256 for auth.
 */

const express = require('express');
const Database = require('better-sqlite3');
const crypto = require('crypto');
const morgan = require('morgan');
const path = require('path');
const fs = require('fs');

// ---- Config (env vars) ----
const PORT = process.env.PORT || 3000;
const HMAC_SECRET = process.env.HMAC_SECRET || '';
const DB_PATH = process.env.DB_PATH || path.join(__dirname, 'hive.db');

if (!HMAC_SECRET) {
  console.warn('WARNING: HMAC_SECRET not set — auth disabled');
}

// ---- Web push (optional — disabled gracefully if web-push isn't installed) ----
let webpush = null;
let VAPID = { publicKey: process.env.VAPID_PUBLIC || '', privateKey: process.env.VAPID_PRIVATE || '' };
try {
  webpush = require('web-push');
  if (!VAPID.publicKey || !VAPID.privateKey) {
    try { VAPID = JSON.parse(fs.readFileSync(path.join(__dirname, 'vapid.json'), 'utf8')); } catch {}
  }
  if (VAPID.publicKey && VAPID.privateKey) {
    webpush.setVapidDetails('mailto:campbellanderson24@gmail.com', VAPID.publicKey, VAPID.privateKey);
    console.log('web push enabled');
  } else {
    console.warn('web push: no VAPID keys — push disabled');
  }
} catch {
  console.warn('web-push not installed — push disabled');
}

// ---- Database ----
const db = new Database(DB_PATH);
db.pragma('journal_mode = WAL');

db.exec(`
  CREATE TABLE IF NOT EXISTS colonies (
    colony_id TEXT PRIMARY KEY,
    last_snapshot TEXT,
    last_snapshot_unix INTEGER DEFAULT 0,
    created_at INTEGER DEFAULT (unixepoch())
  );

  CREATE TABLE IF NOT EXISTS events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    colony_id TEXT NOT NULL,
    tick INTEGER,
    unix INTEGER,
    type TEXT,
    lilguy INTEGER,
    data TEXT,
    raw TEXT,
    FOREIGN KEY (colony_id) REFERENCES colonies(colony_id)
  );

  CREATE INDEX IF NOT EXISTS idx_events_colony_lilguy ON events(colony_id, lilguy, unix);

  CREATE TABLE IF NOT EXISTS commands (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    colony_id TEXT NOT NULL,
    type TEXT NOT NULL,
    payload TEXT,
    created_at INTEGER DEFAULT (unixepoch()),
    acked_at INTEGER DEFAULT 0
  );

  CREATE TABLE IF NOT EXISTS colony_secrets (
    colony_id TEXT PRIMARY KEY,
    secret TEXT NOT NULL,
    enrolled_at INTEGER DEFAULT (unixepoch())
  );

  CREATE TABLE IF NOT EXISTS feedback (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    colony_id TEXT,
    body TEXT NOT NULL,
    context TEXT,
    created_at INTEGER DEFAULT (unixepoch()),
    read_at INTEGER DEFAULT 0
  );

  CREATE TABLE IF NOT EXISTS push_subscriptions (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    colony_id TEXT NOT NULL,
    endpoint TEXT NOT NULL UNIQUE,
    sub TEXT NOT NULL,
    created_at INTEGER DEFAULT (unixepoch())
  );
`);

// Migrations for pre-existing databases (idempotent)
try { db.exec(`ALTER TABLE push_subscriptions ADD COLUMN pref TEXT DEFAULT 'all'`); } catch {}
try { db.exec(`ALTER TABLE colonies ADD COLUMN digest_sent_day TEXT DEFAULT ''`); } catch {}

// Dedup index: remove duplicates first, then create unique index
try {
  db.exec(`CREATE UNIQUE INDEX idx_events_dedup ON events(colony_id, tick, unix, type, lilguy)`);
} catch (e) {
  // Index exists or duplicates prevent creation — clean up and retry
  try {
    db.exec(`DROP INDEX IF EXISTS idx_events_dedup`);
    // Remove duplicates keeping lowest rowid
    db.exec(`
      DELETE FROM events WHERE rowid NOT IN (
        SELECT MIN(rowid) FROM events GROUP BY colony_id, tick, unix, type, lilguy
      )
    `);
    db.exec(`CREATE UNIQUE INDEX idx_events_dedup ON events(colony_id, tick, unix, type, lilguy)`);
    console.log('Dedup index created after cleaning duplicates');
  } catch (e2) {
    console.warn('Could not create dedup index:', e2.message);
  }
}

const stmts = {
  upsertColony: db.prepare(`
    INSERT INTO colonies (colony_id, last_snapshot, last_snapshot_unix)
    VALUES (?, ?, ?)
    ON CONFLICT(colony_id) DO UPDATE SET
      last_snapshot = excluded.last_snapshot,
      last_snapshot_unix = excluded.last_snapshot_unix
  `),
  insertEvent: db.prepare(`
    INSERT OR IGNORE INTO events (colony_id, tick, unix, type, lilguy, data, raw)
    VALUES (?, ?, ?, ?, ?, ?, ?)
  `),
  getSnapshot: db.prepare(`SELECT last_snapshot FROM colonies WHERE colony_id = ?`),
  // Newest N events, returned in chronological order — a plain ASC LIMIT
  // froze the diary at the oldest 200 events once a colony outgrew it
  getEvents: db.prepare(`
    SELECT raw FROM (
      SELECT raw, unix, tick FROM events
      WHERE colony_id = ? AND unix >= ?
      ORDER BY unix DESC, tick DESC LIMIT ?
    ) ORDER BY unix, tick
  `),
  getLilguyEvents: db.prepare(`
    SELECT raw FROM (
      SELECT raw, unix, tick FROM events
      WHERE colony_id = ? AND lilguy = ? AND unix >= ?
      ORDER BY unix DESC, tick DESC LIMIT ?
    ) ORDER BY unix, tick
  `),
  listColonies: db.prepare(`SELECT colony_id, last_snapshot_unix FROM colonies`),
  insertCommand: db.prepare(`
    INSERT INTO commands (colony_id, type, payload) VALUES (?, ?, ?)
  `),
  pendingCommands: db.prepare(`
    SELECT id, type, payload FROM commands
    WHERE colony_id = ? AND acked_at = 0 ORDER BY id LIMIT 16
  `),
  pendingCount: db.prepare(`
    SELECT COUNT(*) AS n FROM commands WHERE colony_id = ? AND acked_at = 0
  `),
  ackCommand: db.prepare(`
    UPDATE commands SET acked_at = unixepoch() WHERE colony_id = ? AND id = ?
  `),
  insertFeedback: db.prepare(`
    INSERT INTO feedback (colony_id, body, context) VALUES (?, ?, ?)
  `),
  feedbackToday: db.prepare(`
    SELECT COUNT(*) AS n FROM feedback WHERE created_at > unixepoch() - 86400
  `),
};

// Command types the app may enqueue (queen applies them on her next poll)
const COMMAND_TYPES = new Set(['name_conker', 'feed_colony', 'set_module_role', 'set_floor_tint', 'gift_care_package', 'ota_update', 'reset_to_satellite']);

// ---- HMAC verification ----
function verifyHmac(body, signature) {
  if (!HMAC_SECRET) return true; // auth disabled
  const expected = crypto
    .createHmac('sha256', HMAC_SECRET)
    .update(body)
    .digest('hex');
  return crypto.timingSafeEqual(Buffer.from(expected), Buffer.from(signature || ''));
}

// ---- Express app ----
const app = express();
app.use(morgan('short'));
app.use(express.raw({ type: 'application/json', limit: '1mb' }));

// Auth middleware for POST endpoints
function authMiddleware(req, res, next) {
  if (!HMAC_SECRET) return next();
  const sig = req.headers['x-hmac-sha256'];
  if (!sig || !verifyHmac(req.body, sig)) {
    return res.status(401).json({ error: 'invalid signature' });
  }
  next();
}

function verifyWith(secret, body, signature) {
  const expected = crypto.createHmac('sha256', secret).update(body).digest('hex');
  try {
    return crypto.timingSafeEqual(Buffer.from(expected), Buffer.from(signature || ''));
  } catch { return false; }
}

// Per-colony auth with self-enrollment (TOFU).
// Order: per-colony secret → legacy global secret → first-contact enrollment
// (unknown colony + X-Enroll-Secret header that signs the body correctly).
function colonyAuth(req, res, next) {
  const cid = req.params.colony_id;
  const sig = req.headers['x-hmac-sha256'];
  const body = req.body;

  const sec = db.prepare(`SELECT secret FROM colony_secrets WHERE colony_id = ?`).get(cid);
  if (sec && sig && verifyWith(sec.secret, body, sig)) return next();

  if (!HMAC_SECRET || (sig && verifyHmac(body, sig))) return next();  // legacy global

  const enroll = req.headers['x-enroll-secret'];
  const known = db.prepare(`SELECT 1 FROM colonies WHERE colony_id = ?`).get(cid);
  if (!sec && !known && typeof enroll === 'string'
      && enroll.length >= 32 && enroll.length <= 128
      && sig && verifyWith(enroll, body, sig)) {
    db.prepare(`INSERT INTO colony_secrets (colony_id, secret) VALUES (?, ?)`).run(cid, enroll);
    console.log(`colony enrolled (TOFU): ${cid}`);
    return next();
  }

  return res.status(401).json({ error: 'invalid signature' });
}

// ---- Web push ----

const TRAIT_LABELS = {
  pioneer: 'Pioneer', elder: 'Elder', bonded: 'Bonded',
  survived_heatwave: 'Heatwave Survivor', survived_cold_snap: 'Cold Snap Survivor',
  survived_drought: 'Drought Survivor', survived_storm: 'Storm Survivor',
  catcher: 'Bug Hunter',
};

// Build a notification for a noteworthy event, or null to skip it.
// tier: 'milestone' reaches everyone; 'ambient' only reaches pref='all'.
function notificationFor(ev) {
  const who = ev.name || 'One of your guys';
  const data = ev.data || {};
  if (ev.type === 'hatch') {
    return { body: `\u{1F331} ${who} hatched!`, tag: 'hatch', tier: 'milestone' };
  }
  if (ev.type === 'death') {
    return { body: `\u{1F342} ${who} passed away.`, tag: 'death', tier: 'milestone' };
  }
  if (ev.type === 'became_best_friends') {
    const target = data.target_name || 'a nestmate';
    return { body: `\u{2B50} ${who} & ${target} are best friends now!`, tag: 'bff', tier: 'milestone' };
  }
  if (ev.type === 'trait_earned') {
    const label = TRAIT_LABELS[data.trait] || data.trait || 'a title';
    return { body: `\u{1F3C5} ${who} earned "${label}"!`, tag: 'trait', tier: 'milestone' };
  }
  if (ev.type === 'challenge_start') {
    const kind = String(data.type || 'challenge').replace(/_/g, ' ');
    return { body: `\u{26A0}\u{FE0F} A ${kind} has hit the colony — they could use a care package.`, tag: 'challenge', tier: 'milestone' };
  }
  if (ev.type === 'challenge_end') {
    const kind = String(data.type || 'challenge').replace(/_/g, ' ');
    return { body: `\u{2600}\u{FE0F} The ${kind} has passed. The survivors will remember.`, tag: 'challenge_end', tier: 'milestone' };
  }
  if (ev.type === 'discovery') {
    const critter = data.critter || 'critter';
    return { body: `${who} found a ${critter}!`, tag: 'discovery', tier: 'ambient' };
  }
  if (ev.type === 'colony_event' && data.kind === 'care_package_from_neighbours') {
    return { body: 'A neighbouring kingdom sent a care package!', tag: 'gift', tier: 'milestone' };
  }
  if (ev.type === 'colony_event' && data.kind === 'eggs_dormant') {
    return { body: 'Your eggs lie dormant for now — more space is needed.', tag: 'eggs_dormant', tier: 'milestone' };
  }
  return null;
}

// Send one payload to every subscription of a colony that passes `filter`.
function sendToSubs(colonyId, payloadObj, filter) {
  if (!webpush || !VAPID.publicKey) return;
  const subs = db.prepare(`SELECT id, sub, pref FROM push_subscriptions WHERE colony_id = ?`).all(colonyId);
  const payload = JSON.stringify(payloadObj);
  for (const row of subs) {
    if (filter && !filter(row.pref || 'all')) continue;
    let sub;
    try { sub = JSON.parse(row.sub); } catch { continue; }
    webpush.sendNotification(sub, payload).catch((err) => {
      if (err.statusCode === 404 || err.statusCode === 410) {
        db.prepare(`DELETE FROM push_subscriptions WHERE id = ?`).run(row.id);  // dead subscription
      }
    });
  }
}

// Fire push notifications for a batch of newly-ingested events (best-effort).
function sendPushesForEvents(colonyId, events) {
  if (!webpush || !VAPID.publicKey) return;
  const seen = new Set();
  const notes = [];
  for (const ev of events) {
    const n = notificationFor(ev);
    if (n && !seen.has(n.tag)) { seen.add(n.tag); notes.push(n); }  // ≤1 per tag per batch
  }
  for (const note of notes) {
    sendToSubs(colonyId,
      { title: 'Hive', body: note.body, tag: note.tag, url: '/app/' },
      // Milestones reach everyone subscribed; ambient only 'all'
      (pref) => note.tier === 'milestone' ? pref !== 'off' : pref === 'all');
  }
}

// ---- Evening digest — "Today in the colony", 8pm local (Europe/London) ----

function londonPart(fmt) {
  return new Intl.DateTimeFormat('en-GB', { timeZone: 'Europe/London', ...fmt });
}

function buildDigestBody(colonyId, todayYmd) {
  const dayFmt = londonPart({ year: 'numeric', month: '2-digit', day: '2-digit' });
  const rows = db.prepare(
    `SELECT raw FROM events WHERE colony_id = ? AND unix >= ? ORDER BY unix ASC`
  ).all(colonyId, Math.floor(Date.now() / 1000) - 86400);
  const events = [];
  for (const r of rows) {
    try {
      const ev = JSON.parse(r.raw);
      if (dayFmt.format(new Date(ev.unix * 1000)) === todayYmd) events.push(ev);
    } catch {}
  }
  if (events.length === 0) return null;  // module offline / nothing at all

  const frags = [];
  const deaths = events.filter(e => e.type === 'death');
  if (deaths.length) frags.push(`\u{1F342} we said goodbye to ${deaths.map(e => e.name || 'a nestmate').slice(0, 2).join(' & ')}`);
  const bff = events.find(e => e.type === 'became_best_friends');
  if (bff) frags.push(`\u{2B50} ${bff.name || 'someone'} & ${(bff.data || {}).target_name || 'a nestmate'} became best friends`);
  const trait = events.find(e => e.type === 'trait_earned');
  if (trait) frags.push(`\u{1F3C5} ${trait.name || 'someone'} earned "${TRAIT_LABELS[(trait.data || {}).trait] || 'a title'}"`);
  const hatches = events.filter(e => e.type === 'hatch');
  if (hatches.length) frags.push(`\u{1F331} ${hatches.length === 1 ? `${hatches[0].name || 'a little one'} hatched` : `${hatches.length} little ones hatched`}`);
  const challenge = events.find(e => e.type === 'challenge_start');
  if (challenge) frags.push(`\u{26A0}\u{FE0F} a ${String((challenge.data || {}).type || 'challenge').replace(/_/g, ' ')} struck`);
  const finds = events.filter(e => e.type === 'discovery').length;
  if (frags.length < 3 && finds > 0) frags.push(`\u{1F50D} ${finds} critter${finds === 1 ? '' : 's'} found`);
  const parades = events.filter(e => e.type === 'play').length;
  if (frags.length < 3 && parades > 0) frags.push(`\u{1F389} ${parades} parade${parades === 1 ? '' : 's'}`);

  if (frags.length === 0) return 'A quiet, contented day in the colony.';
  const body = frags.slice(0, 4).join(' · ');
  return body.charAt(0).toUpperCase() + body.slice(1);
}

setInterval(() => {
  try {
    if (!webpush || !VAPID.publicKey) return;
    const hour = Number(londonPart({ hour: '2-digit', hour12: false }).format(new Date()));
    if (hour < 20) return;
    const todayYmd = londonPart({ year: 'numeric', month: '2-digit', day: '2-digit' }).format(new Date());
    const colonies = db.prepare(`
      SELECT DISTINCT c.colony_id, c.digest_sent_day FROM colonies c
      JOIN push_subscriptions p ON p.colony_id = c.colony_id
    `).all();
    for (const c of colonies) {
      if (c.digest_sent_day === todayYmd) continue;
      db.prepare(`UPDATE colonies SET digest_sent_day = ? WHERE colony_id = ?`).run(todayYmd, c.colony_id);
      const body = buildDigestBody(c.colony_id, todayYmd);
      if (!body) continue;
      sendToSubs(c.colony_id,
        { title: `Today in ${c.colony_id}`, body, tag: 'digest', url: '/app/' },
        (pref) => pref !== 'off');
    }
  } catch (e) {
    console.warn('digest tick failed:', e.message);
  }
}, 60 * 1000);

// Public VAPID key for the app to subscribe with
app.get('/api/v1/push/vapid', (req, res) => {
  res.json({ publicKey: VAPID.publicKey || null });
});

// Register a browser push subscription against a colony.
// Accepts either a bare PushSubscription (legacy) or { sub, pref }.
app.post('/api/v1/colonies/:colony_id/push/subscribe', (req, res) => {
  let parsed;
  try { parsed = JSON.parse(req.body.toString()); } catch { return res.status(400).json({ error: 'invalid json' }); }
  const sub = parsed && parsed.sub ? parsed.sub : parsed;
  const pref = (parsed && parsed.pref) === 'milestones' ? 'milestones' : 'all';
  if (!sub || !sub.endpoint) return res.status(400).json({ error: 'no endpoint' });
  db.prepare(`INSERT INTO push_subscriptions (colony_id, endpoint, sub, pref) VALUES (?, ?, ?, ?)
              ON CONFLICT(endpoint) DO UPDATE SET colony_id = excluded.colony_id, sub = excluded.sub, pref = excluded.pref`)
    .run(req.params.colony_id, sub.endpoint, JSON.stringify(sub), pref);
  res.json({ status: 'subscribed', pref });
});

// Remove a subscription (pref switched to Off in the app)
app.post('/api/v1/colonies/:colony_id/push/unsubscribe', (req, res) => {
  let parsed;
  try { parsed = JSON.parse(req.body.toString()); } catch { return res.status(400).json({ error: 'invalid json' }); }
  if (!parsed || !parsed.endpoint) return res.status(400).json({ error: 'no endpoint' });
  db.prepare(`DELETE FROM push_subscriptions WHERE endpoint = ?`).run(parsed.endpoint);
  res.json({ status: 'unsubscribed' });
});

// ---- Queen push endpoints ----

// POST /api/v1/colonies/:colony_id/snapshot
app.post('/api/v1/colonies/:colony_id/snapshot', colonyAuth, (req, res) => {
  const { colony_id } = req.params;
  const body = req.body.toString();
  let parsed;
  try { parsed = JSON.parse(body); } catch { return res.status(400).json({ error: 'invalid json' }); }

  // Detect the colony entering its population cap (eggs_dormant false -> true) by
  // diffing against the previous snapshot, and push the notice. Snapshots arrive
  // reliably with a valid clock, so this is robust where the one-shot firmware
  // colony_event (emitted at boot, before NTP) is not.
  let wasDormant = false;
  try {
    const prev = stmts.getSnapshot.get(colony_id);
    if (prev && prev.last_snapshot)
      wasDormant = !!(JSON.parse(prev.last_snapshot).population || {}).eggs_dormant;
  } catch {}
  const nowDormant = !!((parsed.population || {}).eggs_dormant);

  stmts.upsertColony.run(colony_id, body, parsed.now_unix || 0);
  res.json({ status: 'ok' });

  if (nowDormant && !wasDormant) {
    sendPushesForEvents(colony_id, [{ type: 'colony_event', data: { kind: 'eggs_dormant' } }]);
  }
});

// POST /api/v1/colonies/:colony_id/events
app.post('/api/v1/colonies/:colony_id/events', colonyAuth, (req, res) => {
  const { colony_id } = req.params;
  const body = req.body.toString();
  let parsed;
  try { parsed = JSON.parse(body); } catch { return res.status(400).json({ error: 'invalid json' }); }

  // Ensure colony row exists without overwriting snapshot data
  db.prepare(`INSERT OR IGNORE INTO colonies (colony_id) VALUES (?)`).run(colony_id);

  const insertMany = db.transaction((events) => {
    const fresh = [];
    for (const ev of events) {
      const info = stmts.insertEvent.run(
        colony_id,
        ev.tick || 0,
        ev.unix || 0,
        ev.type || '',
        ev.lilguy || 0,
        JSON.stringify(ev.data || {}),
        JSON.stringify(ev)
      );
      if (info.changes > 0) fresh.push(ev);  // newly inserted (not a dedup'd repeat)
    }
    return fresh;
  });

  const events = parsed.events || [];
  const fresh = insertMany(events);
  res.json({ status: 'ok', inserted: events.length });
  // Best-effort push for noteworthy new events — after responding
  sendPushesForEvents(colony_id, fresh);
});

// ---- App read endpoints (same shape as queen's local API) ----

// GET /api/v1/colonies
app.get('/api/v1/colonies', (req, res) => {
  const colonies = stmts.listColonies.all();
  res.json({ schema: 1, colonies });
});

// GET /api/v1/colonies/:colony_id
app.get('/api/v1/colonies/:colony_id', (req, res) => {
  const row = stmts.getSnapshot.get(req.params.colony_id);
  if (!row || !row.last_snapshot) return res.status(404).json({ error: 'not found' });
  res.type('application/json').send(row.last_snapshot);
});

// GET /api/v1/colonies/:colony_id/events
app.get('/api/v1/colonies/:colony_id/events', (req, res) => {
  const since = parseInt(req.query.since) || 0;
  const limit = Math.min(parseInt(req.query.limit) || 200, 1000);
  const rows = stmts.getEvents.all(req.params.colony_id, since, limit);
  const results = rows.map(r => JSON.parse(r.raw));
  res.json({ schema: 1, results, next_since: results.length > 0 ? results[results.length - 1].unix : since });
});

// GET /api/v1/colonies/:colony_id/lilguys/:id/events
app.get('/api/v1/colonies/:colony_id/lilguys/:id/events', (req, res) => {
  const since = parseInt(req.query.since) || 0;
  const limit = Math.min(parseInt(req.query.limit) || 200, 1000);
  const rows = stmts.getLilguyEvents.all(req.params.colony_id, parseInt(req.params.id), since, limit);
  const results = rows.map(r => JSON.parse(r.raw));
  res.json({ schema: 1, results });
});

// ---- Command queue (app -> queen) ----

// ---- Firmware OTA ----
// Binaries + a latest.json {version, file, md5} live in FW_DIR. The per-colony
// manifest is HMAC-signed with that colony's secret so a module can trust the
// version/url/md5 even though the binary itself is fetched over plain HTTP.
const FW_DIR = process.env.FW_DIR || '/opt/hive-vps/fw';
const PUBLIC_BASE = process.env.PUBLIC_BASE || 'http://hive.campbell.fish';

function readFwManifest() {
  try { return JSON.parse(fs.readFileSync(path.join(FW_DIR, 'latest.json'), 'utf8')); }
  catch { return null; }
}

// Public: just the latest version (for the app's "update available" check)
app.get('/api/v1/firmware/latest', (req, res) => {
  const m = readFwManifest();
  if (!m) return res.status(404).json({ error: 'no firmware published' });
  res.json({ schema: 1, version: m.version });
});

// The binary itself (HTTP — integrity/authenticity come from the signed md5)
app.get('/api/v1/firmware/bin/:version', (req, res) => {
  const m = readFwManifest();
  if (!m || String(m.version) !== req.params.version) return res.status(404).end();
  const file = path.join(FW_DIR, path.basename(m.file));
  if (!fs.existsSync(file)) return res.status(404).end();
  res.type('application/octet-stream').sendFile(file);
});

// Per-colony signed manifest: {version, url, md5, sig=HMAC(secret, "version\nurl\nmd5")}
app.get('/api/v1/colonies/:colony_id/firmware', (req, res) => {
  const m = readFwManifest();
  if (!m) return res.status(404).json({ error: 'no firmware published' });
  // Sign with the colony's own secret, falling back to the legacy global
  // secret — same order colonyAuth uses to verify the module's requests.
  const sec = db.prepare(`SELECT secret FROM colony_secrets WHERE colony_id = ?`).get(req.params.colony_id);
  const secret = sec ? sec.secret : HMAC_SECRET;
  if (!secret) return res.status(404).json({ error: 'no signing secret' });
  const url = `${PUBLIC_BASE}/api/v1/firmware/bin/${m.version}`;
  const signed = `${m.version}\n${url}\n${m.md5}`;
  const sig = crypto.createHmac('sha256', secret).update(signed).digest('hex');
  res.json({ schema: 1, version: m.version, url, md5: m.md5, sig });
});

// POST /api/v1/colonies/:colony_id/commands  (from app — no HMAC, validated + capped)
app.post('/api/v1/colonies/:colony_id/commands', (req, res) => {
  const { colony_id } = req.params;
  let parsed;
  try { parsed = JSON.parse(req.body.toString()); } catch { return res.status(400).json({ error: 'invalid json' }); }

  if (!COMMAND_TYPES.has(parsed.type)) {
    return res.status(400).json({ error: 'unknown command type' });
  }
  const payload = JSON.stringify(parsed.payload || {});
  if (payload.length > 256) {
    return res.status(400).json({ error: 'payload too large' });
  }
  const pending = stmts.pendingCount.get(colony_id).n;
  if (pending >= 32) {
    return res.status(429).json({ error: 'too many pending commands' });
  }

  const info = stmts.insertCommand.run(colony_id, parsed.type, payload);
  res.json({ status: 'queued', id: info.lastInsertRowid });
});

// GET /api/v1/colonies/:colony_id/commands/pending  (queen polls)
app.get('/api/v1/colonies/:colony_id/commands/pending', (req, res) => {
  const rows = stmts.pendingCommands.all(req.params.colony_id);
  const results = rows.map(r => ({ id: r.id, type: r.type, payload: JSON.parse(r.payload || '{}') }));
  res.json({ schema: 1, results });
});

// POST /api/v1/colonies/:colony_id/commands/ack  (queen, HMAC) body: {ids:[...]}
app.post('/api/v1/colonies/:colony_id/commands/ack', colonyAuth, (req, res) => {
  let parsed;
  try { parsed = JSON.parse(req.body.toString()); } catch { return res.status(400).json({ error: 'invalid json' }); }
  const ids = Array.isArray(parsed.ids) ? parsed.ids : [];
  for (const id of ids) {
    stmts.ackCommand.run(req.params.colony_id, id);
  }
  res.json({ status: 'ok', acked: ids.length });
});

// POST /api/v1/feedback  (from app — no HMAC; length-capped + daily rate cap)
// body: { colony_id, text, context: { fw_version, app_version, ... } }
app.post('/api/v1/feedback', (req, res) => {
  let parsed;
  try { parsed = JSON.parse(req.body.toString()); } catch { return res.status(400).json({ error: 'invalid json' }); }
  const text = String(parsed.text || '').trim();
  if (!text) return res.status(400).json({ error: 'empty feedback' });
  if (text.length > 2000) return res.status(400).json({ error: 'too long (2000 max)' });
  if (stmts.feedbackToday.get().n >= 50) {
    return res.status(429).json({ error: 'feedback inbox full for today' });
  }
  const context = JSON.stringify(parsed.context || {}).slice(0, 512);
  const info = stmts.insertFeedback.run(String(parsed.colony_id || ''), text, context);
  res.json({ status: 'thanks', id: info.lastInsertRowid });
});

// GET /api/v1/health
app.get('/api/v1/health', (req, res) => {
  const colonies = stmts.listColonies.all();
  res.json({ schema: 1, status: 'ok', colonies_count: colonies.length });
});

// ---- Start ----
app.listen(PORT, () => {
  console.log(`Hive VPS server listening on port ${PORT}`);
  console.log(`Database: ${DB_PATH}`);
  console.log(`Auth: ${HMAC_SECRET ? 'enabled' : 'DISABLED (set HMAC_SECRET)'}`);
});
