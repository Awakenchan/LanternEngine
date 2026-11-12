#!/bin/sh
set -eu
cd "$(dirname "$0")"

if [ ! -f .env ]; then
    cp .env.example .env
    printf '%s\n' 'Created .env from .env.example. Set RELAY_DOMAIN and run this script again.'
    exit 1
fi

docker compose run --rm setup
docker compose up -d --build --wait --wait-timeout 180
printf '%s\n' 'Services started. Share invite.txt with your guests.' \
    'Verify public HTTPS at your domain /healthz endpoint.'
