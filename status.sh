#!/usr/bin/env bash
set -uo pipefail

# ── Barvy & symboly ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; DIM='\033[2m'; NC='\033[0m'

OK="[${GREEN}✔${NC}]"; WARN="[${YELLOW}⚠${NC}]"; FAIL="[${RED}✘${NC}]"

COMPOSE_DIR="$(cd "$(dirname "$0")/services" && pwd)"

# ── Helpers ───────────────────────────────────────────────────────────────────
ms() {
    local start end
    start=$(date +%s%N)
    "$@" &>/dev/null
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

http_check() {
    local url=$1 timeout=${2:-3}
    curl -s -o /dev/null -w "%{http_code} %{time_total}" \
        --max-time "$timeout" "$url" 2>/dev/null || echo "000 0"
}

bar() {
    local val=$1 max=$2 width=${3:-20}
    local filled=$(( val * width / (max > 0 ? max : 1) ))
    [ $filled -gt $width ] && filled=$width
    local empty=$(( width - filled ))
    printf "${GREEN}"
    printf '█%.0s' $(seq 1 $filled 2>/dev/null) 2>/dev/null || printf '%0.s█' $(seq 1 $filled)
    printf "${DIM}"
    printf '░%.0s' $(seq 1 $empty 2>/dev/null) 2>/dev/null || printf '%0.s░' $(seq 1 $empty)
    printf "${NC}"
}

# ── Header ────────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${CYAN}║         polycontacts — status check                 ║${NC}"
echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════════════════╝${NC}"
echo -e "  ${DIM}$(date '+%Y-%m-%d %H:%M:%S')${NC}"
echo ""

# ── 1. Docker daemon ──────────────────────────────────────────────────────────
echo -e "${BOLD}── Docker ───────────────────────────────────────────────${NC}"
if ! command -v docker &>/dev/null; then
    echo -e "$FAIL docker není nainstalován"
    exit 1
fi
if ! docker info &>/dev/null; then
    echo -e "$FAIL Docker daemon neběží"
    exit 1
fi
echo -e "$OK  Docker: $(docker --version | grep -oP '\d+\.\d+\.\d+' | head -1)"
echo ""

# ── 2. Kontejnery ─────────────────────────────────────────────────────────────
echo -e "${BOLD}── Kontejnery ───────────────────────────────────────────${NC}"

declare -A SVC_PORTS=(
    [postgres]="5433"
    [gateway-go]="9000"
    [contacts-cpp]="8080"
    [search-rust]="8081"
    [bff-python]="8989"
)

declare -A SVC_HEALTH=(
    [gateway-go]="http://localhost:9000/health"
    [contacts-cpp]="http://localhost:8080/health"
    [search-rust]="http://localhost:8081/health"
    [bff-python]="http://localhost:8989/health"
)

all_up=true
declare -A CONTAINER_STATUS

while IFS= read -r line; do
    svc=$(echo "$line" | awk '{print $1}')
    state=$(echo "$line" | awk '{print $2}')
    health=$(echo "$line" | awk '{print $3}')
    CONTAINER_STATUS[$svc]="${state}:${health}"
done < <(cd "$COMPOSE_DIR" && docker compose ps --format "table {{.Service}}\t{{.State}}\t{{.Health}}" 2>/dev/null | tail -n +2)

for svc in postgres gateway-go contacts-cpp search-rust bff-python; do
    info="${CONTAINER_STATUS[$svc]:-missing:}"
    state="${info%%:*}"
    health="${info##*:}"
    port="${SVC_PORTS[$svc]:-?}"

    if [ "$state" = "running" ]; then
        if [ "$health" = "healthy" ] || [ "$health" = "" ]; then
            icon="$OK"
        elif [ "$health" = "starting" ]; then
            icon="$WARN"
        else
            icon="$FAIL"
            all_up=false
        fi
    else
        icon="$FAIL"
        all_up=false
    fi

    health_label=""
    [ -n "$health" ] && health_label="${DIM} ($health)${NC}"
    printf "  %b  %-16s :%-5s  %b%b\n" "$icon" "$svc" "$port" "${state:-down}" "$health_label"
done
echo ""

# ── 3. HTTP endpointy s latencí ───────────────────────────────────────────────
echo -e "${BOLD}── HTTP endpointy ───────────────────────────────────────${NC}"

declare -A LATENCY
for svc in gateway-go contacts-cpp search-rust bff-python; do
    url="${SVC_HEALTH[$svc]}"
    read -r code time_s <<< "$(http_check "$url")"
    latency_ms=$(echo "$time_s" | awk '{printf "%d", $1 * 1000}')
    LATENCY[$svc]=$latency_ms

    if [ "$code" = "200" ]; then
        icon="$OK"
        lat_color="$GREEN"
        [ "$latency_ms" -gt 200 ] && lat_color="$YELLOW"
        [ "$latency_ms" -gt 500 ] && lat_color="$RED"
    else
        icon="$FAIL"
        lat_color="$RED"
    fi

    printf "  %b  %-16s %s   %b%sms${NC}\n" \
        "$icon" "$svc" "$url" "$lat_color" "$latency_ms"
done
echo ""

# ── 4. PostgreSQL ─────────────────────────────────────────────────────────────
echo -e "${BOLD}── PostgreSQL ───────────────────────────────────────────${NC}"
if docker compose -f "$COMPOSE_DIR/docker-compose.yml" exec -T postgres \
        pg_isready -U contacts &>/dev/null 2>&1; then

    pg_result=$(docker compose -f "$COMPOSE_DIR/docker-compose.yml" exec -T postgres \
        psql -U contacts -d contacts -t -c \
        "SELECT count(*) FROM contacts;" 2>/dev/null | tr -d ' \n' || echo "?")

    pg_phones=$(docker compose -f "$COMPOSE_DIR/docker-compose.yml" exec -T postgres \
        psql -U contacts -d contacts -t -c \
        "SELECT count(*) FROM phone_numbers;" 2>/dev/null | tr -d ' \n' || echo "?")

    echo -e "$OK  PostgreSQL dostupný   host:5433"
    echo -e "    ${DIM}contacts:  ${NC}${BOLD}${pg_result}${NC} ${DIM}záznamů${NC}"
    echo -e "    ${DIM}phones:    ${NC}${BOLD}${pg_phones}${NC} ${DIM}čísel${NC}"
else
    echo -e "$FAIL  PostgreSQL nedostupný"
fi
echo ""

# ── 5. Quick smoke test ───────────────────────────────────────────────────────
echo -e "${BOLD}── Smoke testy ──────────────────────────────────────────${NC}"

smoke() {
    local label=$1 url=$2 expect=$3
    local result
    result=$(curl -s --max-time 3 "$url" 2>/dev/null)
    if echo "$result" | grep -q "$expect"; then
        echo -e "$OK  $label"
    else
        echo -e "$FAIL  $label ${DIM}(expected: '$expect')${NC}"
    fi
}

smoke "contacts list"     "http://localhost:8080/contacts"   '"id"'
smoke "search ping"       "http://localhost:8081/health"     '"ok"'
smoke "gateway services"  "http://localhost:9000/services"   '"contacts"'
smoke "bff proxy"         "http://localhost:8989/api/contacts" '"id"'
echo ""

# ── 6. Logy (poslední chyby) ──────────────────────────────────────────────────
echo -e "${BOLD}── Poslední chyby v logách ──────────────────────────────${NC}"
for svc in gateway-go contacts-cpp search-rust bff-python; do
    errors=$(cd "$COMPOSE_DIR" && docker compose logs --tail=50 "$svc" 2>/dev/null \
        | grep -iE 'error|panic|fatal|exception' \
        | grep -v "health" \
        | tail -2)
    if [ -n "$errors" ]; then
        echo -e "$WARN  ${BOLD}$svc${NC}"
        echo "$errors" | while IFS= read -r line; do
            echo -e "    ${DIM}${line:0:80}${NC}"
        done
    fi
done
echo -e "$OK  logy prohledány"
echo ""

# ── 7. Shrnutí ────────────────────────────────────────────────────────────────
echo -e "${BOLD}── Shrnutí ──────────────────────────────────────────────${NC}"
if $all_up; then
    echo -e "$OK  ${GREEN}${BOLD}Všechny služby běží${NC}"
else
    echo -e "$FAIL  ${RED}${BOLD}Některé služby nejsou dostupné${NC}"
fi
echo ""
echo -e "  Frontend:  ${CYAN}http://localhost:8989${NC}"
echo -e "  Gateway:   http://localhost:9000"
echo -e "  Contacts:  http://localhost:8080"
echo -e "  Search:    http://localhost:8081"
echo ""
echo -e "  ${DIM}Restart:   cd services && docker compose restart <svc>${NC}"
echo -e "  ${DIM}Logy:      cd services && docker compose logs -f <svc>${NC}"
echo ""
