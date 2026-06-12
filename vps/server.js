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

// ---- Config (env vars) ----
const PORT = process.env.PORT || 3000;
const HMAC_SECRET = process.env.HMAC_SECRET || '';
const DB_PATH = process.env.DB_PATH || path.join(__dirname, 'hive.db');

if (!HMAC_SECRET) {
  console.warn('WARNING: HMAC_SECRET not set — auth disabled');
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
`);

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
const COMMAND_TYPES = new Set(['name_conker', 'feed_colony', 'set_module_role', 'set_floor_tint']);

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

// ---- Queen push endpoints ----

// POST /api/v1/colonies/:colony_id/snapshot
app.post('/api/v1/colonies/:colony_id/snapshot', colonyAuth, (req, res) => {
  const { colony_id } = req.params;
  const body = req.body.toString();
  let parsed;
  try { parsed = JSON.parse(body); } catch { return res.status(400).json({ error: 'invalid json' }); }

  stmts.upsertColony.run(colony_id, body, parsed.now_unix || 0);
  res.json({ status: 'ok' });
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
    for (const ev of events) {
      stmts.insertEvent.run(
        colony_id,
        ev.tick || 0,
        ev.unix || 0,
        ev.type || '',
        ev.lilguy || 0,
        JSON.stringify(ev.data || {}),
        JSON.stringify(ev)
      );
    }
  });

  const events = parsed.events || [];
  insertMany(events);
  res.json({ status: 'ok', inserted: events.length });
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
