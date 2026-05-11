#!/usr/bin/env bash
set -euo pipefail

# Spusť na novém stroji pro kompletní nastavení polycontacts projektu.
# Použití:
#   curl -fsSL https://raw.githubusercontent.com/navidofek-cmyk/polycontacts/main/bootstrap.sh | bash
#   nebo: ./bootstrap.sh

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

ok()   { echo -e "${GREEN}  ✔ $*${NC}"; }
warn() { echo -e "${YELLOW}  ⚠ $*${NC}"; }
fail() { echo -e "${RED}  ✘ $*${NC}"; exit 1; }
info() { echo -e "${CYAN}==> $*${NC}"; }

REPO_URL="https://github.com/navidofek-cmyk/polycontacts"
TARGET_DIR="${1:-polycontacts}"

echo ""
echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${CYAN}║     polycontacts — bootstrap             ║${NC}"
echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════╝${NC}"
echo ""

# ── 1. Prerekvizity ───────────────────────────────────────────────────────────
info "Kontrola prerekvizit..."

command -v git    &>/dev/null || fail "git není nainstalován — nainstaluj: sudo apt install git"
command -v docker &>/dev/null || fail "Docker není nainstalován — viz https://docs.docker.com/get-docker/"
docker info       &>/dev/null || fail "Docker daemon neběží — spusť: sudo systemctl start docker"
docker compose version &>/dev/null || fail "docker compose plugin chybí"

ok "git:    $(git --version | head -1)"
ok "docker: $(docker --version | head -1)"
ok "compose: $(docker compose version --short)"

# Volné porty
info "Kontrola portů..."
for port in 8989 9000 8080 8081 5433; do
    if ss -tlnp 2>/dev/null | grep -q ":${port} "; then
        warn "Port $port je obsazený — může dojít ke konfliktu"
    else
        ok "Port $port volný"
    fi
done

# Diskové místo
avail_gb=$(df -BG . | awk 'NR==2 {gsub("G",""); print $4}')
if [ "$avail_gb" -lt 3 ]; then
    warn "Málo místa: ${avail_gb} GB (doporučeno ≥ 3 GB)"
else
    ok "Volné místo: ${avail_gb} GB"
fi

# ── 2. Klonování repozitáře ───────────────────────────────────────────────────
info "Klonuji repozitář..."
if [ -d "$TARGET_DIR" ]; then
    warn "Adresář '$TARGET_DIR' již existuje — přeskakuji clone"
    cd "$TARGET_DIR"
else
    git clone "$REPO_URL" "$TARGET_DIR"
    cd "$TARGET_DIR"
    ok "Repozitář naklonován do: $(pwd)"
fi

# ── 3. Claude Code konfigurace ────────────────────────────────────────────────
info "Nastavuji Claude Code..."

# Projektové nastavení (.claude/settings.local.json)
mkdir -p .claude
if [ ! -f ".claude/settings.local.json" ]; then
    cat > .claude/settings.local.json << 'SETTINGS'
{
  "permissions": {
    "defaultMode": "bypassPermissions"
  },
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write",
        "hooks": [
          {
            "type": "command",
            "command": "file=$(jq -r '.tool_input.file_path // empty' 2>/dev/null); case \"$file\" in *.py) cd services/bff-python && uv run python -m py_compile \"$file\" 2>&1 && echo '{\"systemMessage\": \"✔ Python syntax OK\"}' || echo '{\"systemMessage\": \"✘ Python syntax error\"}' ;; *.go) gofmt -e \"$file\" > /dev/null 2>&1 && echo '{}' || echo '{\"systemMessage\": \"✘ Go syntax error\"}' ;; esac",
            "timeout": 10
          }
        ]
      }
    ],
    "Stop": [
      {
        "matcher": "",
        "hooks": [
          {
            "type": "command",
            "command": "cd services && healthy=$(docker compose ps --format json 2>/dev/null | python3 -c \"import sys,json; rows=[json.loads(l) for l in sys.stdin if l.strip()]; unhealthy=[r.get('Service','?') for r in rows if r.get('Health','') not in ('healthy','')]; print(','.join(unhealthy) if unhealthy else '')\" 2>/dev/null); [ -n \"$healthy\" ] && echo \"{\\\"systemMessage\\\": \\\"⚠ Unhealthy: $healthy\\\"}\" || echo '{}'",
            "timeout": 8
          }
        ]
      }
    ]
  }
}
SETTINGS
    ok "Claude Code projektová konfigurace vytvořena"
else
    ok "Claude Code konfigurace existuje (přeskakuji)"
fi

# Globální nastavení (~/.claude/settings.json)
if [ ! -f "$HOME/.claude/settings.json" ]; then
    mkdir -p "$HOME/.claude"
    cat > "$HOME/.claude/settings.json" << 'GLOBAL'
{
  "model": "sonnet",
  "skipDangerousModePermissionPrompt": true
}
GLOBAL
    ok "Claude Code globální konfigurace vytvořena"
else
    ok "Globální Claude konfigurace existuje (přeskakuji)"
fi

# ── 4. Kontrola Claude Code CLI ──────────────────────────────────────────────
info "Kontrola Claude Code CLI..."
if command -v claude &>/dev/null; then
    ok "Claude Code CLI: $(claude --version 2>/dev/null | head -1 || echo 'dostupný')"
else
    warn "Claude Code CLI není nainstalován"
    echo "  Instalace: npm install -g @anthropic-ai/claude-code"
    echo "  Viz: https://claude.ai/code"
fi

# ── 5. Oprávnění skriptů ──────────────────────────────────────────────────────
info "Nastavuji oprávnění..."
chmod +x start.sh status.sh export.sh bootstrap.sh 2>/dev/null || true
ok "Skripty spustitelné"

# ── 6. Spuštění projektu ──────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}── Spustit projekt? ─────────────────────────────────────${NC}"
read -r -p "  Spustit ./start.sh nyní? [y/N] " answer
case "$answer" in
    [yY]|[yY][eE][sS]|[aA][nN][oO])
        echo ""
        ./start.sh
        ;;
    *)
        echo ""
        info "Spuštění přeskočeno. Spusť ručně:"
        echo ""
        echo -e "  ${CYAN}./start.sh${NC}          ← build + spuštění"
        echo -e "  ${CYAN}./status.sh${NC}         ← kontrola stavu"
        echo ""
        echo -e "  Frontend: http://localhost:8989"
        ;;
esac

# ── Shrnutí ───────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}${GREEN}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${GREEN}║           Bootstrap hotov!               ║${NC}"
echo -e "${BOLD}${GREEN}╚══════════════════════════════════════════╝${NC}"
echo ""
echo -e "  Projekt:  ${BOLD}$(pwd)${NC}"
echo -e "  Frontend: ${CYAN}http://localhost:8989${NC}"
echo -e "  GitHub:   ${CYAN}${REPO_URL}${NC}"
echo ""
echo -e "  Příkazy:"
echo -e "    ./start.sh     ← spustit projekt"
echo -e "    ./status.sh    ← zkontrolovat stav"
echo -e "    ./export.sh    ← exportovat snapshot"
echo -e "    claude         ← otevřít Claude Code v projektu"
echo ""
