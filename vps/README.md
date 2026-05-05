# Hive Colony VPS Server

Receives colony state snapshots and event batches from the queen module via HTTPS POST. Serves the same data back to the companion app via GET endpoints.

## Setup

```bash
cd vps/
npm install
```

## Environment Variables

| Variable | Required | Description |
|----------|----------|-------------|
| `PORT` | No | HTTP port (default: 3000, Caddy proxies from 443) |
| `HMAC_SECRET` | Yes | Shared secret for HMAC-SHA256 auth (same as queen's NVS) |
| `DB_PATH` | No | SQLite database path (default: ./hive.db) |

## Running

```bash
# Development
HMAC_SECRET=your_secret_here npm run dev

# Production (behind Caddy)
HMAC_SECRET=your_secret_here npm start
```

## Caddy Configuration

```
hive.yourdomain.com {
    reverse_proxy localhost:3000
}
```

Or for IP-only with self-signed cert:
```
:443 {
    tls internal
    reverse_proxy localhost:3000
}
```

## systemd Unit

```ini
[Unit]
Description=Hive Colony VPS
After=network.target

[Service]
WorkingDirectory=/opt/hive-vps
ExecStart=/usr/bin/node server.js
Restart=always
Environment=PORT=3000
Environment=HMAC_SECRET=your_secret_here
Environment=DB_PATH=/opt/hive-vps/hive.db

[Install]
WantedBy=multi-user.target
```

## Queen-side Configuration

On the queen's serial console:
```
vps secret <your-base64-secret>
vps endpoint https://178.104.252.49
```

## API Endpoints

### Queen → VPS (POST, requires HMAC)

- `POST /api/v1/colonies/:colony_id/snapshot` — Colony state (every 30s)
- `POST /api/v1/colonies/:colony_id/events` — Event batch

### App → VPS (GET, no auth for v1)

- `GET /api/v1/health` — Server health
- `GET /api/v1/colonies` — List all colonies
- `GET /api/v1/colonies/:colony_id` — Latest snapshot for a colony
- `GET /api/v1/colonies/:colony_id/events?since=&limit=` — Events
- `GET /api/v1/colonies/:colony_id/lilguys/:id/events?since=&limit=` — Per-LilGuy events

## Database

SQLite with WAL mode. Tables:
- `colonies` — colony_id, last_snapshot (full JSON), timestamps
- `events` — per-event rows with colony_id, tick, unix, type, lilguy, raw JSON

Indexes on (colony_id, unix) and (colony_id, lilguy, unix) for efficient queries.
