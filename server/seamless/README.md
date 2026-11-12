# Private co-op

Based on Lantern Engine by Awaken (https://github.com/Awakenchan/LanternEngine).
Modified, unofficial package. One room, up to 5 players; protocol 17.

**Requirements:** Docker Compose, a domain pointing directly to your VPS, and TCP
443 available and allowed through the firewall. Your frontend keeps port 80.
Caddy manages HTTPS certificates automatically.

Extract the ZIP into a clean folder, keeping `server/` and `engine/`. Copy your
existing `.env` and `invite.txt`, if any. From `server/seamless`:

```sh
cp .env.example .env  # first installation only
nano .env             # set RELAY_DOMAIN
sudo sh start-docker.sh
```

The `.env` file sets the domain, Docker project name, and log rotation. Setup
generates `ROOM_ID` while preserving those settings. Commit `.env.example` to Git;
`.env`, local variants, and `invite.txt` are ignored.

On the first run, paste the launcher's **Online → Seamless → Host → Copy invite code**
code. Later runs reuse `invite.txt`.

On the host, enable online play and Seamless, disable direct connections, and set:

```ini
seamless_server=https://YOUR_DOMAIN
```

Guests import the updated `invite.txt`. This file contains the room password.

```sh
curl --fail https://YOUR_DOMAIN/healthz
sudo docker compose logs -f relay caddy
sudo docker compose down
```

Expected response: `relay reachable`. Connections appear as `slot ... joined`.
If the build fails, capture the error with `sudo docker compose --progress plain build relay`.
Files already use their final names; no renaming is needed.
