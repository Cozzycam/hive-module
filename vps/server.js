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
  getEvents: db.prepare(`
    SELECT raw FROM events WHERE colony_id = ? AND unix >= ? ORDER BY unix, tick LIMIT ?
  `),
  getLilguyEvents: db.prepare(`
    SELECT raw FROM events WHERE colony_id = ? AND lilguy = ? AND unix >= ? ORDER BY unix, tick LIMIT ?
  `),
  listColonies: db.prepare(`SELECT colony_id, last_snapshot_unix FROM colonies`),
};

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

// ---- Queen push endpoints ----

// POST /api/v1/colonies/:colony_id/snapshot
app.post('/api/v1/colonies/:colony_id/snapshot', authMiddleware, (req, res) => {
  const { colony_id } = req.params;
  const body = req.body.toString();
  let parsed;
  try { parsed = JSON.parse(body); } catch { return res.status(400).json({ error: 'invalid json' }); }

  stmts.upsertColony.run(colony_id, body, parsed.now_unix || 0);
  res.json({ status: 'ok' });
});

// POST /api/v1/colonies/:colony_id/events
app.post('/api/v1/colonies/:colony_id/events', authMiddleware, (req, res) => {
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
