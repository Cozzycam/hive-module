#!/bin/bash
set -e
cd "$(dirname "$0")"
npm install
npm run build
ssh hive@178.104.252.49 "sudo mkdir -p /opt/hive-app/dist"
rsync -avz --delete dist/ hive@178.104.252.49:/opt/hive-app/dist/
ssh hive@178.104.252.49 "sudo systemctl reload caddy"
echo "Deployed to https://hive.campbell.fish/app/"
