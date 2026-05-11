#!/usr/bin/env bash
set -euo pipefail

# ── Barvy ─────────────────────────────────────────────────────────────────────
GREEN='\033[0;32m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
ok()   { echo -e "${GREEN}  ✔ $*${NC}"; }
info() { echo -e "${CYAN}==> $*${NC}"; }

PROJ_DIR="$(cd "$(dirname "$0")" && pwd)"
DATE=$(date +%Y-%m-%d)
EXPORT_NAME="polycontacts-export-${DATE}"
EXPORT_DIR="/tmp/${EXPORT_NAME}"
ARCHIVE="${PROJ_DIR}/${EXPORT_NAME}.tar.gz"

echo ""
echo -e "${BOLD}${CYAN}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${CYAN}║     polycontacts — export projektu       ║${NC}"
echo -e "${BOLD}${CYAN}╚══════════════════════════════════════════╝${NC}"
echo ""

# ── 1. Příprava adresáře ──────────────────────────────────────────────────────
info "Připravuji export adresář..."
rm -rf "$EXPORT_DIR"
mkdir -p "$EXPORT_DIR"

# ── 2. Zdrojový kód ───────────────────────────────────────────────────────────
info "Kopíruji zdrojový kód..."
rsync -a --relative \
    --exclude='.git' \
    --exclude='.venv' \
    --exclude='__pycache__' \
    --exclude='*.pyc' \
    --exclude='target/' \
    --exclude='build/' \
    --exclude='*.o' \
    --exclude='*.a' \
    --exclude='node_modules' \
    --exclude='site/' \
    --exclude='*.tar.gz' \
    --exclude='services/gateway-go/gateway-go' \
    --exclude='chat_history/*.jsonl' \
    "$PROJ_DIR/." "$EXPORT_DIR/source/"

ok "Zdrojový kód zkopírován"

# ── 3. Claude Code konfigurace ────────────────────────────────────────────────
info "Exportuji Claude Code konfiguraci..."
mkdir -p "$EXPORT_DIR/claude-config"

# settings.local.json
cp "$PROJ_DIR/.claude/settings.local.json" \
   "$EXPORT_DIR/claude-config/settings.local.json"

# CLAUDE.md
cp "$PROJ_DIR/CLAUDE.md" "$EXPORT_DIR/claude-config/CLAUDE.md"

# Globální nastavení (pokud existuje)
if [ -f "$HOME/.claude/settings.json" ]; then
    cp "$HOME/.claude/settings.json" "$EXPORT_DIR/claude-config/global-settings.json"
fi

ok "Claude Code konfigurace exportována"

# ── 4. Databáze dump ──────────────────────────────────────────────────────────
info "Databázový dump (pokud Docker běží)..."
if cd "$PROJ_DIR/services" && docker compose ps postgres 2>/dev/null | grep -q "running"; then
    docker compose exec -T postgres \
        pg_dump -U contacts contacts > "$EXPORT_DIR/contacts_dump.sql" 2>/dev/null \
        && ok "PostgreSQL dump: contacts_dump.sql" \
        || echo "  ⚠ dump selhal, pokračuji bez něj"
else
    echo "  ⚠ PostgreSQL neběží — dump přeskočen"
fi

# ── 5. Git bundle ─────────────────────────────────────────────────────────────
info "Vytvářím git bundle (celá historie)..."
git -C "$PROJ_DIR" bundle create "$EXPORT_DIR/polycontacts.bundle" --all 2>/dev/null \
    && ok "Git bundle: polycontacts.bundle" \
    || echo "  ⚠ git bundle selhal"

# ── 6. README pro export ──────────────────────────────────────────────────────
info "Generuji EXPORT_README.md..."
COMMIT=$(git -C "$PROJ_DIR" rev-parse --short HEAD 2>/dev/null || echo "?")
BRANCH=$(git -C "$PROJ_DIR" branch --show-current 2>/dev/null || echo "?")

cat > "$EXPORT_DIR/EXPORT_README.md" << EOF
# polycontacts — Export ${DATE}

Commit: \`${COMMIT}\`  Branch: \`${BRANCH}\`

## Obsah tohoto exportu

\`\`\`
polycontacts-export-${DATE}/
├── source/              ← kompletní zdrojový kód (bez build artefaktů)
├── claude-config/       ← Claude Code nastavení
│   ├── settings.local.json   ← hooks + bypassPermissions
│   ├── CLAUDE.md             ← projektový kontext
│   └── global-settings.json  ← globální Claude nastavení (pokud byl exportován)
├── contacts_dump.sql    ← PostgreSQL dump (pokud Docker běžel při exportu)
├── polycontacts.bundle  ← git bundle s celou historií
└── EXPORT_README.md     ← tento soubor
\`\`\`

## Jak obnovit projekt na novém stroji

### Možnost A — z git bundle (zachová historii)
\`\`\`bash
git clone polycontacts.bundle polycontacts
cd polycontacts
\`\`\`

### Možnost B — z GitHub
\`\`\`bash
git clone https://github.com/navidofek-cmyk/polycontacts
cd polycontacts
\`\`\`

### Možnost C — ze source/ adresáře (bez git)
\`\`\`bash
cp -r source/ polycontacts
cd polycontacts
\`\`\`

---

## Nastavení Claude Code

\`\`\`bash
# 1. Zkopírovat projektové nastavení
mkdir -p polycontacts/.claude
cp claude-config/settings.local.json polycontacts/.claude/

# 2. Zkopírovat globální nastavení (volitelně)
cp claude-config/global-settings.json ~/.claude/settings.json

# 3. Ověřit CLAUDE.md (je součástí source/)
cat polycontacts/CLAUDE.md
\`\`\`

## Obnovení databáze

Pokud existuje \`contacts_dump.sql\`:
\`\`\`bash
cd polycontacts/services
docker compose up -d postgres
sleep 5
docker compose exec -T postgres psql -U contacts contacts < ../../contacts_dump.sql
\`\`\`

## Spuštění projektu

\`\`\`bash
cd polycontacts
./start.sh
\`\`\`

Frontend: http://localhost:8989

## Prerekvizity

- Docker ≥ 24
- docker compose plugin
- 2+ GB volného místa
- Porty 8080, 8081, 8989, 9000, 5433 volné
EOF

ok "EXPORT_README.md vygenerován"

# ── 7. Vytvoření archivu ──────────────────────────────────────────────────────
info "Vytvářím archiv..."
tar -czf "$ARCHIVE" -C /tmp "$EXPORT_NAME"
SIZE=$(du -sh "$ARCHIVE" | cut -f1)

rm -rf "$EXPORT_DIR"

# ── Výsledek ──────────────────────────────────────────────────────────────────
echo ""
echo -e "${BOLD}${GREEN}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}${GREEN}║             Export hotov!                ║${NC}"
echo -e "${BOLD}${GREEN}╚══════════════════════════════════════════╝${NC}"
echo ""
echo -e "  Archiv:  ${BOLD}${ARCHIVE}${NC}"
echo -e "  Velikost: ${SIZE}"
echo ""
echo -e "  Obsah:"
echo -e "    source/          ← zdrojový kód"
echo -e "    claude-config/   ← Claude Code konfigurace"
echo -e "    polycontacts.bundle  ← git historie"
echo -e "    contacts_dump.sql    ← databáze (pokud běžela)"
echo ""
echo -e "  Rozbalit: ${CYAN}tar -xzf $(basename "$ARCHIVE")${NC}"
echo ""
