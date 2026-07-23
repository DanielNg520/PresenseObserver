# Deploy Plan — PresenseObserver on DigitalOcean

Brief deployment plan for the **shared $4 droplet** that also hosts `miki_a_friendly_sorter_bot`.
Both projects live on one box via Docker Compose to split the cost.

## Target box
- **DigitalOcean Basic Droplet**, $4/mo — 512 MB RAM, 1 vCPU, 10 GB disk, Ubuntu 24.04
- **No backups/snapshots** (they bill extra — the surprise charge last time)
- Region close to your ESP32s / users

## ⚠️ Memory blocker: MySQL 8.0 won't fit
`server/docker-compose.yml` currently uses `mysql:8.0`, which idles at ~400 MB — alone
that blows the 512 MB budget. **Pick one before deploying:**

1. **Switch to MariaDB** (`image: mariadb:11`, ~120 MB) — near drop-in, recommended.
2. **Use SQLite** if the schema is simple — drops the DB container entirely, lightest.
3. Tune MySQL with `--innodb-buffer-pool-size=64M` etc. — fiddly, still heavy.

Recommendation: **MariaDB**. Update the `image:` and keep the rest of compose as-is.

## One-time droplet setup (shared with miki)
```bash
# 1. Add 2 GB swap — essential on a 512 MB box, prevents OOM kills
sudo fallocate -l 2G /swapfile && sudo chmod 600 /swapfile
sudo mkswap /swapfile && sudo swapon /swapfile
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab

# 2. Install Docker + Compose plugin
curl -fsSL https://get.docker.com | sh

# 3. Firewall: SSH, HTTPS, and MQTT TLS
sudo ufw allow 22 && sudo ufw allow 443 && sudo ufw allow 8883 && sudo ufw enable
```

## Deploy steps
1. `git clone` this repo to `/opt/presenseobserver` (or `scp` it up).
2. Create `server/.env` with `DB_*`, `MQTT_BROKER`, `MQTT_TOPIC` (see `server/.env`).
3. Add **Mosquitto** to the compose stack (broker the ESP32s connect to on 8883):
   - mount config + Let's Encrypt certs for `observer.duynq.com`
   - expose `1883` (internal) and `8883` (TLS, public)
4. Add **memory limits** so one service can't starve the others:
   ```yaml
   deploy:
     resources:
       limits:
         memory: 200M   # webserver
   ```
5. `docker compose up -d --build`

## Networking
- Point `observer.duynq.com` → droplet IP via Cloudflare (DNS only / grey cloud for MQTT).
- **Nginx** container terminates HTTPS for the dashboard/WebSocket → proxies to `webserver:8000`.
- This Nginx is **shared with miki** if miki ever switches to webhook mode (currently polling, so not needed).
- TLS via Let's Encrypt (`certbot` on host or an `nginx-proxy`/`acme` container).

## Resource budget (512 MB + 2 GB swap)
| Service | ~RAM |
|---|---|
| Ubuntu + Docker | ~220 MB |
| Mosquitto | ~15 MB |
| MariaDB | ~120 MB |
| webserver (FastAPI) | ~120 MB |
| Nginx | ~15 MB |
| miki (polling) | ~100 MB |
| **Total** | **~590 MB** → swap absorbs the overflow |

Tight but workable for low traffic. If containers get OOM-killed, resize to the $6 / 1 GB tier
(DO resize is in-place, no rebuild).

## TODO before this is live (from CLAUDE.md)
- [ ] Mosquitto TLS (8883) config
- [ ] FastAPI backend written
- [ ] Frontend written
- [ ] Swap to MariaDB / SQLite in compose
