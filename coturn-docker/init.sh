#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

STAGING=""
if [ "$1" = "--staging" ]; then
    STAGING="--staging"
    echo "*** STAGING MODE: using Let's Encrypt staging server ***"
    echo "*** Certificates will NOT be trusted by browsers     ***"
    echo ""
fi

# ── Pre-flight checks ────────────────────────────────────────────

if ! command -v docker &>/dev/null; then
    echo "ERROR: Docker is not installed."
    echo "Install with: curl -fsSL https://get.docker.com | sh"
    exit 1
fi

if ! docker compose version &>/dev/null; then
    echo "ERROR: Docker Compose v2 is not available."
    echo "Update Docker or install the compose plugin."
    exit 1
fi

if ! docker info &>/dev/null 2>&1; then
    echo "ERROR: Docker daemon is not running, or you need sudo."
    echo "Try: sudo ./init.sh"
    exit 1
fi

# ── Configuration ─────────────────────────────────────────────────

if [ ! -f .env ]; then
    echo "============================================"
    echo "  First-time setup: creating .env file"
    echo "============================================"
    cp .env.example .env

    read -p "Enter your domain name (e.g. turn.example.com): " domain
    read -p "Enter your email for Let's Encrypt: " email
    read -p "Enter PUBLISH_URL (https://..., empty to disable): " publish_url

    turn_secret=$(openssl rand -hex 32)

    sed -i "s|DOMAIN=.*|DOMAIN=$domain|" .env
    sed -i "s|EMAIL=.*|EMAIL=$email|" .env
    sed -i "s|TURN_SECRET=.*|TURN_SECRET=$turn_secret|" .env
    awk -v p="$publish_url" '/^PUBLISH_URL=/{ print "PUBLISH_URL=" p; next }1' .env > .env.tmp && mv .env.tmp .env

    echo ""
    echo "Configuration saved to .env"
    echo "TURN_SECRET generated automatically."
fi

source .env

echo ""
echo "============================================"
echo "  Domain:  $DOMAIN"
echo "  Email:   $EMAIL"
if [ -n "$PUBLISH_URL" ]; then
    echo "  Publish: $PUBLISH_URL"
else
    echo "  Publish: (disabled)"
fi
echo "  Auth:    TURN REST API (ephemeral credentials)"
echo "============================================"
echo ""

# ── DNS check ─────────────────────────────────────────────────────

if command -v dig &>/dev/null; then
    RESOLVED_IP=$(dig +short "$DOMAIN" | tail -1)
    if [ -z "$RESOLVED_IP" ]; then
        echo "WARNING: $DOMAIN does not resolve to any IP address."
        echo "Make sure the DNS A record is set before continuing."
        read -p "Continue anyway? [y/N] " confirm
        [ "$confirm" = "y" ] || [ "$confirm" = "Y" ] || exit 1
    else
        echo "DNS check: $DOMAIN -> $RESOLVED_IP"
    fi
fi

# ── Port check ────────────────────────────────────────────────────

check_port() {
    if ss -tlnp 2>/dev/null | grep -q ":$1 "; then
        echo "WARNING: Port $1 is already in use:"
        ss -tlnp | grep ":$1 "
        read -p "Continue anyway? [y/N] " confirm
        [ "$confirm" = "y" ] || [ "$confirm" = "Y" ] || exit 1
    fi
}

ACME_PORT="${ACME_PORT:-80}"

check_port "$ACME_PORT"
check_port 3478
check_port 5349

# ── Build ─────────────────────────────────────────────────────────

echo ""
echo "Step 1: Building coturn image..."
docker compose build

# ── Certificate ───────────────────────────────────────────────────

echo ""
echo "Step 2: Generating Let's Encrypt certificate..."

echo "  Using certbot standalone (port $ACME_PORT must be reachable from the internet)"
echo ""

docker compose run --rm --entrypoint "" \
    -p $ACME_PORT:80 \
    certbot \
    certbot certonly \
        --standalone \
        --preferred-challenges http \
        --email "$EMAIL" \
        --agree-tos \
        --no-eff-email \
        $STAGING \
        -d "$DOMAIN"

# ── Start ─────────────────────────────────────────────────────────

echo ""
echo "Step 3: Starting services..."
docker compose up -d

echo ""
echo "============================================"
echo "  Coturn server is running!"
echo "============================================"
echo ""
echo "  STUN:   stun:$DOMAIN:3478"
echo "  TURN:   turn:$DOMAIN:3478 (UDP/TCP)"
echo "  TURNS:  turns:$DOMAIN:5349 (TLS)"
echo ""
echo "  Auth:   TURN REST API (ephemeral credentials)"
echo "  Secret: ${TURN_SECRET:0:8}...  (full value in .env)"
echo ""
echo "  Ping test page: https://$DOMAIN:8000/"
echo "  Ping API:       https://$DOMAIN:8000/ping"
echo ""
echo "  Your backend must generate credentials using TURN_SECRET."
echo "  See README.md for Node.js / Python examples."
echo ""
echo "  Verify health:  docker compose ps"
echo "  View logs:      docker compose logs -f coturn"
echo ""
if [ -n "$STAGING" ]; then
echo "  *** STAGING certificates — not browser-trusted ***"
echo "  To switch to production certs, keep .env and the volumes and run:"
echo "    docker compose run --rm --no-deps --entrypoint \"\" certbot \\"
echo "      certbot certonly --webroot -w /var/www/certbot --force-renewal \\"
echo "      --email \"$EMAIL\" --agree-tos --no-eff-email -d \"$DOMAIN\""
echo "    docker compose restart nginx coturn"
echo "  (README: Switch from staging to production certificates)"
echo ""
fi
