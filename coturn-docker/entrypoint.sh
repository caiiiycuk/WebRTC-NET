#!/bin/bash
set -e

: "${DOMAIN:?ERROR: DOMAIN is required}"
: "${TURN_SECRET:?ERROR: TURN_SECRET is required (generate with: openssl rand -hex 32)}"

if [ -z "$PUBLIC_IP" ]; then
    PUBLIC_IP=$(curl -s -4 --connect-timeout 5 https://api.ipify.org \
             || curl -s -4 --connect-timeout 5 https://ifconfig.me \
             || curl -s -4 --connect-timeout 5 https://checkip.amazonaws.com \
             || true)
    PUBLIC_IP=$(echo "$PUBLIC_IP" | tr -d '[:space:]')
    : "${PUBLIC_IP:?ERROR: Could not detect public IP. Set PUBLIC_IP in .env}"
    echo "Detected public IP: $PUBLIC_IP"
fi

export PUBLIC_IP TURN_SECRET
export RELAY_PORT_MIN="${RELAY_PORT_MIN:-49152}"
export RELAY_PORT_MAX="${RELAY_PORT_MAX:-65535}"
export TOTAL_QUOTA="${TOTAL_QUOTA:-600}"
export USER_QUOTA="${USER_QUOTA:-600}"

CERT_DIR="/etc/letsencrypt/live/$DOMAIN"
CONF="/etc/turnserver.conf"
COTURN_RESTART_EXPECTED="/var/lib/coturn/restart-expected"

generate_config() {
    envsubst < /etc/turnserver.conf.template > "$CONF"
    if [ -f "$CERT_DIR/fullchain.pem" ] && [ -f "$CERT_DIR/privkey.pem" ]; then
        printf '\ncert=%s/fullchain.pem\npkey=%s/privkey.pem\n' "$CERT_DIR" "$CERT_DIR" >> "$CONF"
        echo "TLS: certificates loaded from $CERT_DIR"
    else
        echo "WARNING: no TLS certs at $CERT_DIR — TURNS unavailable. Run ./init.sh"
    fi
}

TURNSERVER_PID=""

start_turnserver() {
    turnserver -c "$CONF" --pidfile /var/run/turnserver.pid -v &
    TURNSERVER_PID=$!
}

# Watch for cert renewal by polling fullchain.pem mtime.
# The certbot-certs volume is mounted read-only here, so we must not rely on
# deleting a flag file — instead we detect that certbot rewrote the cert.
watch_certs() {
    local cert="$CERT_DIR/fullchain.pem"
    local last_mtime=""
    [ -f "$cert" ] && last_mtime=$(stat -c %Y "$cert" 2>/dev/null || echo "")
    while true; do
        sleep 60
        [ -f "$cert" ] || continue
        local mtime
        mtime=$(stat -c %Y "$cert" 2>/dev/null || echo "")
        [ -n "$mtime" ] && [ "$mtime" != "$last_mtime" ] || continue
        last_mtime="$mtime"
        echo "Certificate changed — restarting turnserver..."
        generate_config
        touch "$COTURN_RESTART_EXPECTED"
        kill -TERM "$(cat /var/run/turnserver.pid 2>/dev/null)" 2>/dev/null || true
    done
}

mkdir -p /var/lib/coturn
generate_config

trap 'kill 0 2>/dev/null || true; wait 2>/dev/null || true; exit 0' TERM INT

watch_certs &
[ -n "$PUBLISH_URL" ] && python3 /usr/local/bin/publisher.py &

echo "Starting coturn..."
while true; do
    start_turnserver
    wait "$TURNSERVER_PID" || true
    if [ -f "$COTURN_RESTART_EXPECTED" ]; then
        rm -f "$COTURN_RESTART_EXPECTED"
        echo "turnserver stopped for certificate reload — restarting..."
        continue
    fi
    echo "ERROR: turnserver exited unexpectedly" >&2
    kill 0 2>/dev/null || true
    exit 1
done
