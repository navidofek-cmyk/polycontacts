#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✔ $*${NC}"; }
warn() { echo -e "${YELLOW}  ⚠ $*${NC}"; }
fail() { echo -e "${RED}  ✘ $*${NC}"; exit 1; }
info() { echo -e "${CYAN}==> $*${NC}"; }

COMPOSE_DIR="$(cd "$(dirname "$0")/services" && pwd)"

# ── 1. Předpoklady ─────────────────────────────────────────────────────────────
info "Kontrola předpokladů..."

command -v docker &>/dev/null       || fail "docker není nainstalován"
command -v docker &>/dev/null && ok "docker nalezen: $(docker --version | head -1)"

docker info &>/dev/null             || fail "Docker daemon neběží — spusť Docker Desktop nebo 'sudo systemctl start docker'"
ok "Docker daemon běží"

docker compose version &>/dev/null  || fail "docker compose plugin chybí"
ok "docker compose: $(docker compose version --short)"

# ── 2. Volné porty ─────────────────────────────────────────────────────────────
info "Kontrola portů..."
for port in 8989 9000 8080 8081; do
    if ss -tlnp 2>/dev/null | grep -q ":${port} " || \
       lsof -iTCP:"$port" -sTCP:LISTEN -t &>/dev/null 2>&1; then
        warn "Port $port je obsazený — může dojít ke konfliktu"
    else
        ok "Port $port je volný"
    fi
done

# ── 3. Diskové místo ───────────────────────────────────────────────────────────
info "Diskové místo..."
avail_gb=$(df -BG / | awk 'NR==2 {gsub("G",""); print $4}')
if [ "$avail_gb" -lt 2 ]; then
    warn "Málo místa na disku: ${avail_gb} GB — build může selhat"
else
    ok "Volné místo: ${avail_gb} GB"
fi

# ── 4. Existující kontejnery ───────────────────────────────────────────────────
info "Kontrola existujících kontejnerů..."
cd "$COMPOSE_DIR"
running=$(docker compose ps --services --filter status=running 2>/dev/null | tr '\n' ' ')
if [ -n "$running" ]; then
    warn "Běžící služby: $running — budu je restartovat"
    docker compose down --remove-orphans
else
    ok "Žádné kolidující kontejnery"
fi

# ── 5. Build a spuštění ────────────────────────────────────────────────────────
info "Sestavuji obrazy a spouštím..."
docker compose up --build -d

# ── 6. Čekání na healthy ───────────────────────────────────────────────────────
info "Čekám na zdravotní kontroly (max 120 s)..."
timeout=120
elapsed=0
while [ $elapsed -lt $timeout ]; do
    all_healthy=true
    for svc in gateway-go contacts-cpp search-rust bff-python; do
        status=$(docker compose ps --format json 2>/dev/null \
            | python3 -c "
import sys, json
for line in sys.stdin:
    try:
        d = json.loads(line)
        if '${svc}' in d.get('Service','') or '${svc}' in d.get('Name',''):
            print(d.get('Health', d.get('Status','unknown')))
            break
    except: pass
else:
    print('missing')
" 2>/dev/null || echo "unknown")
        if [[ "$status" != "healthy" ]]; then
            all_healthy=false
        fi
    done
    $all_healthy && break
    sleep 3
    elapsed=$((elapsed + 3))
    echo -n "."
done
echo ""

# ── 7. Finální stav ────────────────────────────────────────────────────────────
info "Stav služeb:"
docker compose ps

echo ""
info "Živost endpointů:"
for url in \
    "http://localhost:9000/health  gateway-go" \
    "http://localhost:8080/health  contacts-cpp" \
    "http://localhost:8081/health  search-rust" \
    "http://localhost:8989/health  bff-python"; do
    endpoint=$(echo "$url" | awk '{print $1}')
    name=$(echo "$url" | awk '{print $2}')
    code=$(curl -s -o /dev/null -w "%{http_code}" --max-time 3 "$endpoint" 2>/dev/null || echo "000")
    if [ "$code" = "200" ]; then
        ok "$name → $endpoint ($code)"
    else
        warn "$name → $endpoint ($code)"
    fi
done

echo ""
echo -e "${GREEN}  Frontend:  http://localhost:8989${NC}"
echo -e "  Gateway:   http://localhost:9000"
echo -e "  Contacts:  http://localhost:8080"
echo -e "  Search:    http://localhost:8081"
echo ""
echo -e "  Zastavení: cd services && docker compose down"
