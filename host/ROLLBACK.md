# Rollback point — phone-module live rollout

Created before pointing the phone module at the production VPS. Three parts.

## Rollback timestamp
`20260706-231415`  (VPS backups live in `/opt/hive-vps/backups/`)

## 1. Code
All phone-module work is on branch `phone-module-wasm`. `main` (`3a71058`) is the
known-good pre-rollout state.
```
git checkout main          # revert local repo to pre-rollout
```

## 2. Production database  (`/opt/hive-vps/hive.db`)
Atomic backup (includes WAL): `/opt/hive-vps/backups/hive.db.rollback-20260706-231415`
(integrity_check: ok, 18 colonies). To restore:
```
ssh hive
sudo systemctl stop hive-vps          # stop the node service (unit name per systemd)
cd /opt/hive-vps
cp hive.db hive.db.broken             # keep the bad one for inspection
rm -f hive.db-wal hive.db-shm          # drop stale WAL
cp backups/hive.db.rollback-20260706-231415 hive.db
sudo systemctl start hive-vps
```
NOTE: restoring the DB also discards any legitimate hardware-module pushes since
the backup. For a short rollout window that's fine; otherwise prefer surgical
cleanup (below) over a full restore.

### Surgical cleanup (usually enough — additive rollout)
Phone colonies are new rows; delete them without touching existing data:
```
sqlite3 /opt/hive-vps/hive.db "DELETE FROM colonies WHERE colony_id LIKE 'app-%';
  DELETE FROM colony_secrets WHERE colony_id LIKE 'app-%';
  DELETE FROM events WHERE colony_id LIKE 'app-%';"
```
(Phone colony_ids are minted `app-<hex>` by host/web/push.js.)

## 3. Companion app  (`/opt/hive-app/dist`, served at hive.campbell.fish/app/)
Backup: `/opt/hive-vps/backups/dist.rollback-20260706-231415.tgz`. To restore:
```
ssh hive
rm -rf /opt/hive-app/dist
tar -C /opt/hive-app -xzf /opt/hive-vps/backups/dist.rollback-20260706-231415.tgz
```

## 4. server.js  (not changed by this rollout, backed up defensively)
`/opt/hive-vps/backups/server.js.rollback-20260706-231415` → copy to
`/opt/hive-vps/server.js` and restart if ever needed.
