# Coturn Docker — STUN / TURN / TURNS with Let's Encrypt

Dockerized [coturn](https://github.com/coturn/coturn) server (built from the [caiiiycuk/coturn](https://github.com/caiiiycuk/coturn) fork, see [Coturn base image](#coturn-base-image)) with automatic TLS certificate management via Let's Encrypt, and a browser-based latency test page.

## What's included


| Service     | Purpose                                                      |
| ----------- | ------------------------------------------------------------ |
| **coturn**  | STUN, TURN (UDP/TCP), and TURNS (TLS) server                 |
| **nginx**   | HTTP ping API with CORS, ACME challenge responder, test page |
| **certbot** | Automatic Let's Encrypt certificate issuance and renewal     |


### Ports


| Port        | Protocol  | Service                              |
| ----------- | --------- | ------------------------------------ |
| 80          | TCP       | ACME challenges only (Let's Encrypt) |
| 3478        | TCP + UDP | STUN and TURN                        |
| 5349        | TCP       | TURNS — TLS relay                    |
| 8000        | TCP       | HTTPS — ping API + test page         |
| 49152–65535 | UDP       | Media relay range (configurable)     |


## VPS requirements

### Recommended specs


| Tier           | vCPUs | RAM   | Use case                                    |
| -------------- | ----- | ----- | ------------------------------------------- |
| **Minimum**    | 1     | 1 GB  | Testing, a handful of concurrent sessions   |
| **Small**      | 2     | 2 GB  | Up to ~50 concurrent TURN relays            |
| **Production** | 4+    | 4+ GB | 100+ concurrent relays, headroom for spikes |


STUN is nearly free (stateless, tiny packets). TURN is the resource hog — each relayed media stream consumes CPU for packet forwarding and bandwidth proportional to the media bitrate. Budget roughly **~0.5 MB/s per audio+video relay** and **~1 % CPU per relay** on a modern core.

Disk space is negligible (< 1 GB for the image, certs, and logs).

### Recommended OS

- **Ubuntu 22.04 LTS** or **24.04 LTS** (best Docker support, widely tested)
- Debian 12 also works well
- Avoid Alpine or minimal images as the host OS — they can lack kernel modules needed for advanced networking

### Prerequisites

- A VPS with a public IP (e.g. Digital Ocean droplet)
- A domain name with an **A record** pointing to the VPS IP
- Docker and Docker Compose installed

### Install Docker (if needed)

```bash
curl -fsSL https://get.docker.com | sh
```

## Linux kernel tuning

TURN relays large volumes of UDP traffic. The default kernel settings are too conservative for production. Apply these tunings on the **host** (not inside the container).

### Apply tunings (run as root)

```bash
cat >> /etc/sysctl.d/99-coturn.conf << 'EOF'
# ── UDP buffer sizes ──────────────────────────────────────────────
# Default rmem/wmem are 208 KB — far too small for bursty media relay.
# Set receive and send buffers to 4 MB default, 16 MB max.
net.core.rmem_default = 4194304
net.core.rmem_max     = 16777216
net.core.wmem_default = 4194304
net.core.wmem_max     = 16777216

# ── Network backlog ───────────────────────────────────────────────
# How many packets can queue before the kernel starts dropping.
# Default is 1000; raise for high-throughput relay.
net.core.netdev_budget     = 600
net.core.netdev_max_backlog = 8192

# ── Conntrack ─────────────────────────────────────────────────────
# Each TURN allocation creates conntrack entries. Default 65536 is
# too low for heavy relay usage.
net.netfilter.nf_conntrack_max = 262144

# ── Ephemeral port range ──────────────────────────────────────────
# Widen the local port range so the kernel doesn't run out when
# coturn opens many relay sockets.
net.ipv4.ip_local_port_range = 1024 65535

# ── TIME_WAIT ─────────────────────────────────────────────────────
# Recycle TIME_WAIT sockets faster for TCP-based TURN/TURNS.
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_fin_timeout = 15

# ── File descriptors ──────────────────────────────────────────────
# Each relay session needs multiple file descriptors.
fs.file-max = 1048576
EOF

sysctl --system
```

### Raise file descriptor limits

```bash
cat >> /etc/security/limits.d/99-coturn.conf << 'EOF'
*    soft    nofile    1048576
*    hard    nofile    1048576
root soft    nofile    1048576
root hard    nofile    1048576
EOF
```

Also set the Docker daemon default (create or edit `/etc/docker/daemon.json`):

```json
{
  "default-ulimits": {
    "nofile": { "Name": "nofile", "Hard": 1048576, "Soft": 1048576 }
  }
}
```

Then restart Docker:

```bash
systemctl restart docker
```

### Verify tunings

```bash
sysctl net.core.rmem_max net.core.wmem_max net.core.netdev_max_backlog
ulimit -n
```

## Quick start

### 1. Clone and enter the directory

```bash
cd coturn-docker
```

### 2. Open firewall ports

```bash
ufw allow 80/tcp       # ACME challenges (Let's Encrypt)
ufw allow 3478/tcp     # STUN/TURN
ufw allow 3478/udp     # STUN/TURN
ufw allow 5349/tcp     # TURNS
ufw allow 8000/tcp     # HTTPS ping API + test page
ufw allow 49152:65535/udp  # Media relay
```

If you use Digital Ocean's cloud firewall, create equivalent inbound rules there as well.

### 3. Run the init script

**For testing** (uses Let's Encrypt staging server, no rate limits):

```bash
./init.sh --staging
```

**For production** (real trusted certificate):

```bash
./init.sh
```

The script will:

1. Check that Docker is installed, daemon is running, and ports are free
2. Verify DNS resolves your domain
3. Ask for your **domain**, **email**, **TURN username**, and **password**
4. Save configuration to `.env`
5. Build the coturn Docker image
6. Obtain a Let's Encrypt TLS certificate (port 80 must be reachable)
7. Start all services

> **Let's Encrypt rate limits:** Production certificates are limited to 5 duplicate certs per domain per week and 50 certs per registered domain per week. Always use `--staging` first to make sure everything works, then re-run without it. To switch from staging to production: `rm .env && docker compose down -v && ./init.sh`

### 4. Verify it's running

```bash
docker compose ps
docker compose logs coturn --tail 50
```

Visit `https://your-domain:8000/` in a browser to open the latency test page.

## Manual setup (without init.sh)

```bash
cp .env.example .env
# Edit .env with your values
nano .env

# Get certificate
docker compose run --rm --entrypoint "" -p 80:80 certbot \
  certbot certonly --standalone --preferred-challenges http \
  --email you@example.com --agree-tos --no-eff-email -d turn.example.com

# Start services
docker compose up -d
```

## Configuration

All settings are in the `.env` file:


| Variable           | Description                                                                                                                          | Default      |
| ------------------ | ------------------------------------------------------------------------------------------------------------------------------------ | ------------ |
| `DOMAIN`           | Domain name pointing to your VPS                                                                                                     | *(required)* |
| `EMAIL`            | Email for Let's Encrypt notifications                                                                                                | *(required)* |
| `TURN_SECRET`      | Shared secret for TURN REST API authentication. Your backend generates ephemeral credentials from this. Auto-generated by `init.sh`. | *(required)* |
| `ACME_PORT`        | Host port for ACME challenges. Set to a non-standard port (e.g. `9080`) when proxying through a host nginx.                          | `80`         |
| `RELAY_PORT_MIN`   | Start of UDP relay port range                                                                                                        | `49152`      |
| `RELAY_PORT_MAX`   | End of UDP relay port range                                                                                                          | `65535`      |
| `TOTAL_QUOTA`      | Maximum concurrent TURN allocations across the server                                                                                | `9000`        |
| `USER_QUOTA`       | Maximum concurrent TURN allocations for one REST username                                                                            | `128`        |
| `PUBLIC_IP`        | VPS public IP (auto-detected if empty)                                                                                               | *(auto)*     |
| `PUBLISH_URL`      | URL to POST iceServers + traffic JSON to (empty = disabled)                                                                          | *(empty)*    |
| `PUBLISH_INTERVAL` | Seconds between iceServers publications                                                                                              | `60`         |
| `PUBLISH_LABEL`    | Optional label added to published JSON for backend-side identification                                                               | *(empty)*    |
| `PUBLISH_FLAG`     | Optional flag added to published JSON                                                                                                | *(empty)*    |
| `PUBLISH_MAX_TRAFFIC_GB` | Optional maximum traffic value in GB added to published JSON                                                                    | *(empty)*    |


### Published JSON format

When `PUBLISH_URL` is set, the entrypoint POSTs the following JSON every `PUBLISH_INTERVAL` seconds:

```json
{
  "iceServers": [
    { "urls": ["stun:turn.example.com:3478"] },
    {
      "urls": ["turn:turn.example.com:3478", "turns:turn.example.com:5349"],
      "username": "1740000000:session-id",
      "credential": "base64-hmac..."
    }
  ],
  "traffic": {
    "dailyBytes": 123456789,
    "monthlyBytes": 9876543210
  },
  "label": "eu-west-1",
  "flag": "eu",
  "maxTrafficGb": 1024
}
```

The `traffic` object reports total bytes relayed by coturn (received + sent), tracked via coturn's built-in prometheus metrics. Counters reset at midnight (daily) and on the 1st of each month. Stats persist across container restarts via the `coturn-data` volume.

`label`, `flag`, and `maxTrafficGb` are optional additive fields. They are included only when `PUBLISH_LABEL`, `PUBLISH_FLAG`, and `PUBLISH_MAX_TRAFFIC_GB` are set, so installations that leave all three variables empty keep publishing the legacy JSON shape unchanged.

The published `turnSecret` is intended for a trusted backend. Do not cache and serve the same generated `username`/`credential` pair to all browsers unless `USER_QUOTA` is sized for all concurrent allocations using that shared username. The safer production pattern is to mint a fresh REST username per client/session request.

### Proxying ACME through host nginx

If port 80 is already used by a host nginx, you can proxy the ACME challenges through it to the coturn-docker nginx on a non-standard port.

#### 1. Set `ACME_PORT` in `.env`

```
ACME_PORT=9080
```

#### 2. Add a host nginx config

Create `/etc/nginx/sites-available/turn.example.com`:

```nginx
# ACME proxy + reverse proxy to coturn HTTPS ping
server {
    listen 80;
    server_name turn.example.com;

    # Proxy ACME challenges to coturn-docker nginx
    location /.well-known/acme-challenge/ {
        proxy_pass http://127.0.0.1:9080;
        proxy_set_header Host $host;
    }

    # Redirect everything else to HTTPS
    location / {
        return 301 https://$host$request_uri;
    }
}

server {
    listen 443 ssl http2;
    server_name turn.example.com;

    # After init.sh generates the cert, point to it:
    ssl_certificate /etc/letsencrypt/live/turn.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/turn.example.com/privkey.pem;
    ssl_protocols TLSv1.2 TLSv1.3;

    # Reverse proxy the ping API and test page
    location / {
        proxy_pass https://127.0.0.1:8000;
        proxy_ssl_verify off;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
    }
}
```

Enable it:

```bash
ln -s /etc/nginx/sites-available/turn.example.com /etc/nginx/sites-enabled/
nginx -t && systemctl reload nginx
```

#### 3. Run init

```bash
./init.sh
```

`init.sh` detects `ACME_PORT != 80` and switches to webroot mode automatically — it starts the coturn-docker nginx, then runs certbot through your host proxy chain.

#### 4. Share certificates with host nginx (optional)

If you want the host nginx `443` block to use the same Let's Encrypt cert, copy it out of the Docker volume:

```bash
# Find the volume path
docker volume inspect coturn-docker_certbot-certs --format '{{ .Mountpoint }}'

# Symlink or copy to /etc/letsencrypt (if not already there)
# Alternatively, mount the volume path directly in the host nginx config
```

Or mount the Docker volume path in your host nginx config:

```nginx
ssl_certificate /var/lib/docker/volumes/coturn-docker_certbot-certs/_data/live/turn.example.com/fullchain.pem;
ssl_certificate_key /var/lib/docker/volumes/coturn-docker_certbot-certs/_data/live/turn.example.com/privkey.pem;
```

> **Note:** Renewals work through the same proxy — the certbot service uses webroot mode, and your host nginx forwards the challenge to the coturn-docker nginx on `ACME_PORT`.

## Usage in your application

This server uses **TURN REST API** authentication (RFC 5766 extension). Your backend generates short-lived credentials using `TURN_SECRET`, passes them to the client, and they expire automatically. No static passwords are ever exposed in client code.

### 1. Backend generates credentials

**Node.js:**

```javascript
const crypto = require("crypto");

function getTurnCredentials(secret, ttlSeconds = 86400, identity = crypto.randomUUID()) {
  const timestamp = Math.floor(Date.now() / 1000) + ttlSeconds;
  const username = `${timestamp}:${identity}`;
  const hmac = crypto.createHmac("sha1", secret);
  hmac.update(username);
  const credential = hmac.digest("base64");
  return { username, credential };
}

// Express example
app.get("/api/turn-credentials", (req, res) => {
  const identity = req.sessionID || crypto.randomUUID();
  const creds = getTurnCredentials(process.env.TURN_SECRET, 86400, identity);
  res.json({
    ...creds,
    urls: [
      "stun:turn.example.com:3478",
      "turn:turn.example.com:3478",
      "turns:turn.example.com:5349"
    ]
  });
});
```

**Python:**

```python
import hmac, hashlib, base64, time

def get_turn_credentials(secret: str, ttl: int = 86400, identity: str | None = None):
    timestamp = int(time.time()) + ttl
    username = f"{timestamp}:{identity or str(time.time_ns())}"
    dig = hmac.new(secret.encode(), username.encode(), hashlib.sha1).digest()
    credential = base64.b64encode(dig).decode()
    return {"username": username, "credential": credential}
```

### 2. Client fetches credentials and connects

```javascript
const { username, credential, urls } = await fetch("/api/turn-credentials").then(r => r.json());

const pc = new RTCPeerConnection({
  iceServers: [{ urls, username, credential }]
});
```

Credentials are valid for 24 hours by default (the `ttlSeconds` parameter). Coturn validates the HMAC signature and expiry timestamp automatically — no database needed.

### Measuring latency from the browser

#### HTTP ping

```javascript
async function pingTurnServer(domain) {
  // Warm-up (establish connection)
  await fetch(`https://${domain}:8000/ping`, { mode: "cors", cache: "no-store" });

  const samples = [];
  for (let i = 0; i < 5; i++) {
    const t0 = performance.now();
    await fetch(`https://${domain}:8000/ping`, { mode: "cors", cache: "no-store" });
    samples.push(performance.now() - t0);
  }
  samples.sort((a, b) => a - b);
  return Math.round(samples[Math.floor(samples.length / 2)]); // median ms
}

const latency = await pingTurnServer("turn.example.com");
console.log(`HTTP round-trip: ${latency}ms`);
```

#### STUN latency (more accurate for WebRTC)

```javascript
function pingSTUN(domain) {
  return new Promise((resolve) => {
    const pc = new RTCPeerConnection({
      iceServers: [{ urls: `stun:${domain}:3478` }]
    });
    const t0 = performance.now();
    let done = false;

    pc.onicecandidate = (e) => {
      if (done) return;
      if (e.candidate && e.candidate.candidate.includes("srflx")) {
        done = true;
        pc.close();
        resolve(Math.round(performance.now() - t0));
      }
    };

    pc.createDataChannel("ping");
    pc.createOffer().then((o) => pc.setLocalDescription(o));

    setTimeout(() => {
      if (!done) { done = true; pc.close(); resolve(null); }
    }, 5000);
  });
}

const stunLatency = await pingSTUN("turn.example.com");
console.log(`STUN round-trip: ${stunLatency}ms`);
```

### Test page

Open `https://your-domain:8000/` in a browser for an interactive test that measures HTTPS, STUN, and TURN relay latency with a visual UI.

## Certificate renewal

Certificates renew automatically every 12 hours via the certbot service. Both services pick up the renewed certificate on their own, without a manual step:

- **coturn** polls the certificate's modification time and restarts `turnserver` when it changes (the `certbot-certs` volume is read-only inside coturn, so it cannot rely on a flag file).
- **nginx** reloads itself every 6 hours, so it never keeps serving a stale in-memory certificate (which is what causes `ERR_CERT_DATE_INVALID` on `:8000` long after the cert was actually renewed on disk).

To trigger a manual renewal:

```bash
./renew-certs.sh
```

That script runs `certbot renew`, reloads nginx so it serves the new TLS certificates, then restarts the coturn container so it reloads certs as well (same outcome as the automatic certbot renewal hook).

## Changing configuration

### Rotate the TURN secret

```bash
NEW_SECRET=$(openssl rand -hex 32)
sed -i "s|TURN_SECRET=.*|TURN_SECRET=$NEW_SECRET|" .env
docker compose up -d --force-recreate coturn
echo "New secret: $NEW_SECRET"
# Update your backend with the new secret
```

### Change relay ports

Edit `.env`, then restart coturn:

```bash
nano .env
docker compose up -d --force-recreate coturn
```

### Change domain name

A new domain requires a new certificate:

```bash
docker compose down
nano .env   # update DOMAIN and EMAIL

# Remove old certificates
docker volume rm coturn-docker_certbot-certs

# Re-generate
docker compose run --rm --entrypoint "" -p 80:80 certbot \
  certbot certonly --standalone --preferred-challenges http \
  --email you@example.com --agree-tos --no-eff-email -d new.example.com

docker compose up -d
```

### Expand relay port range

Edit `RELAY_PORT_MIN` / `RELAY_PORT_MAX` in `.env`, update the firewall rule, then restart:

```bash
ufw allow 49152:65535/udp
docker compose up -d --force-recreate coturn
```

## Security notes

### What's already hardened

- **TURN REST API auth**: No static passwords. Ephemeral credentials expire automatically (default 24h). The shared secret never leaves the server.
- **TLS**: TLSv1.0 and TLSv1.1 are disabled; only TLSv1.2+ with strong cipher suites
- **Relay lockdown**: All RFC 1918 private ranges, loopback, link-local, and reserved IP blocks are denied as relay peers (prevents SSRF)
- **Rate limiting**: The `/ping` endpoint is rate-limited to 10 req/s per IP with a burst of 20
- **HSTS**: Strict-Transport-Security header is set on the HTTPS ping endpoint
- **No CLI**: The coturn admin CLI is disabled
- **Session limits**: Per-user quota of 32 concurrent allocations, 600 total

### Additional recommendations

**Protect your TURN_SECRET.** This is the only authentication credential. If compromised, rotate it immediately: generate a new one with `openssl rand -hex 32`, update `.env`, and restart coturn.

**Restrict the ping test page in production.** If you don't need the browser test page publicly, block port 8000 in your firewall.

**Keep images updated.** Periodically pull new base images:

```bash
docker compose pull
docker compose build --pull --no-cache
docker compose up -d
```

**Consider fail2ban.** If you see brute-force authentication attempts in coturn logs, install fail2ban on the host and add a coturn jail. See the monitoring section below for log inspection commands.

## Monitoring

### Health checks

Health checks are built into docker-compose. View status with:

```bash
docker compose ps
```

A healthy output looks like:

```
NAME              STATUS                  PORTS
coturn-server     running (healthy)
coturn-nginx      running (healthy)       0.0.0.0:80->80/tcp, 0.0.0.0:8000->8000/tcp
coturn-certbot    running
```

### Live session count

Coturn logs every allocation. Count active TURN sessions:

```bash
docker compose logs coturn --since 5m 2>&1 | grep -c "allocation created"
```

### Bandwidth monitoring

Use `vnstat` on the host for real-time traffic stats:

```bash
apt install -y vnstat
vnstat -l -i eth0
```

Or check per-container stats with Docker:

```bash
docker stats --no-stream
```

### Testing STUN from the command line

The coturn image includes `turnutils_stunclient`:

```bash
docker compose exec coturn turnutils_stunclient your-domain
```

### Testing TURN from the command line

Generate ephemeral credentials, then test:

```bash
# Generate credentials (valid for 24h)
SECRET=$(grep TURN_SECRET .env | cut -d= -f2)
TIMESTAMP=$(($(date +%s) + 86400))
USERNAME="$TIMESTAMP:cli-test-$(date +%s)"
CREDENTIAL=$(echo -n "$USERNAME" | openssl dgst -sha1 -hmac "$SECRET" -binary | base64)

echo "Username: $USERNAME"
echo "Credential: $CREDENTIAL"

# Test TURN allocation
docker compose exec coturn turnutils_uclient -u "$USERNAME" -w "$CREDENTIAL" your-domain
```

## Log management

Docker logs are capped at **3 files x 10 MB** per container (configured in `docker-compose.yml`). This prevents disk exhaustion.

View logs:

```bash
docker compose logs coturn --tail 100
docker compose logs nginx --tail 100
docker compose logs certbot --tail 100

# Follow all logs live
docker compose logs -f
```

To keep coturn less verbose in production, remove the `verbose` line from `turnserver.conf.template` and restart:

```bash
docker compose up -d --force-recreate coturn
```

## Backup and restore

Only two things need backing up:


| What             | Where                         | Command               |
| ---------------- | ----------------------------- | --------------------- |
| Configuration    | `.env`                        | `cp .env .env.backup` |
| TLS certificates | Docker volume `certbot-certs` | See below             |


### Back up certificates

```bash
docker run --rm -v coturn-docker_certbot-certs:/certs -v $(pwd):/backup \
  alpine tar czf /backup/certs-backup.tar.gz -C /certs .
```

### Restore certificates

```bash
docker run --rm -v coturn-docker_certbot-certs:/certs -v $(pwd):/backup \
  alpine sh -c "cd /certs && tar xzf /backup/certs-backup.tar.gz"
```

If you lose the certificates, simply re-run `./init.sh` — certbot will issue new ones.

## Updating

### Update coturn and nginx images

```bash
docker compose pull
docker compose build --pull --no-cache
docker compose up -d
```

### Coturn base image

`Dockerfile` builds on `ghcr.io/caiiiycuk/coturn:<coturn-ver>-<N>`, a prebuilt `linux/amd64` image of the [caiiiycuk/coturn](https://github.com/caiiiycuk/coturn) fork. Servers only download it; coturn is never compiled on the server. The fork sources are the `3rdparty/coturn` submodule of HumbleNet.

Rule: the committed `3rdparty/coturn` submodule commit is the commit of the `release/<tag>` whose image is in `FROM`.

Releasing a new image:

1. Merge the changes into `master` of the fork.
2. Tag and push: `git tag release/<coturn-ver>-<N> && git push origin release/<coturn-ver>-<N>`. The `Release image` workflow (`.github/workflows/release-image.yml` in the fork) builds `docker/coturn/debian/Dockerfile` and pushes `ghcr.io/caiiiycuk/coturn:<coturn-ver>-<N>`.
3. Wait for the workflow to finish and check that `docker pull ghcr.io/caiiiycuk/coturn:<coturn-ver>-<N>` works without login (the package must be public).
4. In HumbleNet, update `FROM` in `Dockerfile`, check out the tag in `3rdparty/coturn`, and commit both together.

Updating servers: `git pull && docker compose build --pull && docker compose up -d`.

### Update certbot

```bash
docker compose pull certbot
docker compose up -d certbot
```

## Stopping and restarting

```bash
# Stop all services
docker compose down

# Start again
docker compose up -d

# Restart a single service
docker compose restart coturn

# View logs
docker compose logs -f
```

## Troubleshooting

**Certificate generation fails**

- Ensure port 80 is open and reachable from the internet
- Verify the domain A record resolves to your VPS IP: `dig +short your-domain`
- Check certbot logs: `docker compose logs certbot`

**nginx fails to start (certificate not found)**

- This happens if you run `docker compose up` before generating certificates
- Run `./init.sh` for first-time setup, or generate certs manually (see "Manual setup" above)

**TURNS not working**

- Verify certificates exist: `docker compose exec coturn ls /etc/letsencrypt/live/your-domain/`
- Check coturn logs: `docker compose logs coturn`
- Ensure ports 5349 and 443 are open
- Test with: `openssl s_client -connect your-domain:5349`

**No relay candidates in browser**

- Verify your backend is generating credentials with the correct `TURN_SECRET`
- Check if credentials have expired (default TTL is 24h)
- Ensure UDP ports 49152–65535 are open on both host firewall and cloud firewall
- Test with the built-in test page at `https://your-domain:8000/` (enter your TURN_SECRET)
- Test from CLI using the ephemeral credential generation commands in the Monitoring section above

**Firefox says "ICE failed, your TURN server appears to be broken"**

- In `about:webrtc`, check the TURN allocation errors. A first `401` response is normal REST auth challenge.
- If the retry fails with STUN error `486`, coturn is rejecting allocation because the quota for that REST username is exhausted.
- This usually means many clients are sharing the same generated `username`/`credential` pair. Generate credentials per client/session, or raise `USER_QUOTA` and `TOTAL_QUOTA` in `.env`, then restart coturn with `docker compose up -d --force-recreate coturn`.

**Coturn fails to start**

- Check if ports 3478 or 5349 are already in use by another service
- Check if the public IP was detected correctly: `docker compose logs coturn | grep "public IP"`

**High packet loss or poor quality**

- Apply the kernel tunings from the "Linux kernel tuning" section above
- Check if UDP buffers are sufficient: `sysctl net.core.rmem_max`
- Monitor with `docker stats` — if CPU is consistently above 80%, upgrade the VPS

**Health check failing**

- `docker compose ps` shows "unhealthy"
- For coturn: check if port 3478 is reachable — `docker compose exec coturn turnutils_stunclient localhost`
- For nginx: check config — `docker compose exec nginx nginx -t`
