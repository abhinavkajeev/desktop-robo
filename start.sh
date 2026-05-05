#!/usr/bin/env bash
# Robi launcher — runs server + listener in two terminals
# Usage: bash start.sh [server-ip]  (defaults to 127.0.0.1)

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

# Activate venv
source .venv/bin/activate

SERVER_IP="${1:-127.0.0.1}"
SERVER_URL="http://${SERVER_IP}:5001"

echo ""
echo "╔══════════════════════════════════════╗"
echo "║        🤖  Robi Launcher             ║"
echo "╠══════════════════════════════════════╣"
echo "║  Server  : $SERVER_URL"
echo "╚══════════════════════════════════════╝"
echo ""

# ── Terminal 1: Flask server ──────────────────────────────────────────────
osascript -e "tell application \"Terminal\" to do script \"cd '$SCRIPT_DIR' && source .venv/bin/activate && python robi.py\""

sleep 2   # give the server a moment to start

# ── Terminal 2: Laptop client (wake-word listener) ────────────────────────
osascript -e "tell application \"Terminal\" to do script \"cd '$SCRIPT_DIR' && source .venv/bin/activate && python laptop_client.py --server $SERVER_URL --model base\""

echo "✅  Both processes started in new Terminal windows."
echo "   Say 'Hey Robi' to activate!"
