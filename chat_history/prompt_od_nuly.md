# Reverse Engineering Spec — Contacts Microservices

> Dokument vznikl zpětnou analýzou běžícího systému a zdrojového kódu.
> Slouží jako kompletní podklad pro reimplementaci od nuly.

---

## 0. Claude Code konfigurace pro tento projekt

Před zahájením práce nastavit Claude Code pro autonomní a parallelní práci.

### `.claude/settings.local.json` (projektová, negitovat)
```json
{
  "permissions": {
    "defaultMode": "bypassPermissions"
  }
}
```
`bypassPermissions` — Claude nečeká na potvrzení před každým příkazem. Nutné pro plynulou autonomní práci. Nastavuje se pouze lokálně (settings.local.json), ne globálně.

### Globální `~/.claude/settings.json`
```json
{
  "model": "sonnet",
  "skipDangerousModePermissionPrompt": true
}
```

### Jak zadat práci pro parallelní agenty
Claude Code podporuje spuštění více agentů paralelně. Pro velké úkoly (např. testy pro 4 různé služby najednou) formulovat prompt takto:

```
Spusť 4 agenty paralelně — každý napíše testy pro jednu službu:
- Agent 1: pytest testy pro bff-python (mock httpx)
- Agent 2: go test pro gateway-go (httptest)
- Agent 3: cargo test pro search-rust (tower::oneshot)
- Agent 4: vlastní C++ runner pro contacts-cpp
```

Claude pak spustí všechny najednou přes `Agent()` tool a počká na výsledky.

### Efektivní prompty pro tento projekt
Místo: *"napiš testy"*
Napsat: *"napiš pytest testy pro FastAPI BFF — mockuj httpx přes vlastní transport, testuj všechny /api/* endpointy, spusť `uv run pytest -v` a oprav chyby"*

Klíčové: zahrnout **jak spustit** a **co opravit při chybě** — agent pak pracuje autonomně bez dotazů.

---

## Pozorované chování systému (black box)

### Vstupní bod
Jediný veřejně přístupný port je **8989**. Ostatní porty (8080, 8081, 9000) jsou interní — přístupné přes Docker síť i z hostitele, ale klienti je přímo nevolají.

### Co systém dělá
- CRUD pro kontakty (jméno, příjmení, email, telefony, kategorie)
- Full-text vyhledávání s relevancí (příjmení má vyšší váhu než email)
- Self-monitoring: každá služba reportuje stav, latenci, počet requestů
- Topologie: gateway drží graf "kdo volá koho"

### Startup sekvence (pozorovatelná zvenku)
1. postgres nastartuje první, otevře port 5432 (a gateway-go paralelně)
2. gateway-go nastartuje, otevře port 9000
3. contacts-cpp nastartuje (čeká na postgres healthy + gateway-go healthy), zaregistruje se u gateway, port 8080 — schema se vytvoří automaticky, seed data se vloží pokud je tabulka prázdná
4. search-rust nastartuje, stáhne kontakty z contacts-cpp, zaindexuje je, zaregistruje se u gateway, port 8081
5. bff-python nastartuje, zaregistruje se u gateway, port 8989
6. Systém je ready — `GET /health` na všech portech vrací 200

---

## Architektura (white box)

```
[Browser]
    │ HTTP :8989
    ▼
[bff-python]  FastAPI/Python
    │
    ├──────────────────────────────► [contacts-cpp]  C++20  :8080
    │   /api/contacts → /contacts                     CRUD, PostgreSQL store
    │   /api/contacts/{id} → /contacts/{id}           shared_mutex, thread pool 8
    │                                │                     │
    │                                │ POST /index         │ libpqxx
    │                                │ (při každém zápisu) ▼
    │                                │             [postgres]  :5432
    │                                │              postgres_data volume
    │                                ▼
    ├──────────────────────────────► [search-rust]  Rust/Axum  :8081
    │   /api/search?q= → /search?q=                   invertovaný index, weighted
    │
    └──────────────────────────────► [gateway-go]  Go  :9000
        /api/services → /services                     service registry
        /api/topology → /topology                     graf závislostí
        /api/stats    → fan-out na všechny           parallel asyncio.gather
```

### Co gateway NENÍ
Gateway není na kritické cestě pro data. BFF volá contacts-cpp a search-rust **přímo** (ne přes gateway proxy). Gateway slouží výhradně jako registry a monitoring.

---

## Datové struktury

### Contact
```json
{
  "id":         "550e8400-e29b-41d4-a716-446655440000",
  "first_name": "Jana",
  "last_name":  "Nováková",
  "email":      "jana@example.com",
  "phones":     [{"label": "mobil", "number": "+420 601 111 222"}],
  "category":   "Friend"
}
```

**Invarianty:**
- `id` je vždy UUID v4 (`xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`), generuje ho contacts-cpp při POST
- `first_name` a `last_name` jsou required — prázdné vrátí 400
- `phones` může být prázdný array
- `category` má defaultní hodnotu `"Other"` pokud chybí
- Povolené kategorie: `Friend`, `Work`, `Family`, `Other`, `Colleague`

### ServiceEntry (gateway interní)
```json
{
  "name": "contacts",
  "url": "http://contacts-cpp:8080",
  "health_path": "/health",
  "status": "healthy",
  "latency_ms": 0,
  "last_checked": "2026-05-10T10:00:00Z",
  "registered_at": "2026-05-10T09:59:43Z",
  "request_count": 42,
  "error_count": 0
}
```

---

## Kontrakty jednotlivých služeb

### contacts-cpp (:8080)

```
GET  /contacts          ?q=<substring>
                        → 200 array, sorted dle (last_name ASC, first_name ASC)
                        → filtr je case-insensitive substring přes first_name+last_name+email+category
                        → bez q= vrátí vše

GET  /contacts/{id}     → 200 object | 404 {"error":"contact {id} not found"}

POST /contacts          body: Contact bez id
                        → 201 object s vygenerovaným id
                        → 400 pokud chybí first_name nebo last_name
                        → side effect: async POST /index na search-rust

PUT  /contacts/{id}     body: Contact bez id
                        → 200 object (id zůstane zachováno z URL)
                        → 404 | 400
                        → side effect: async POST /index na search-rust

DELETE /contacts/{id}   → 204 | 404

GET  /health            → 200 {"status":"ok","service":"contacts-cpp","threads":8}
GET  /stats             → 200 {"requests":N,"errors":N,"uptime_s":N}
```

**Chování při chybě search notifikace:** ignoruje se — fire and forget, detached thread

### search-rust (:8081)

```
POST /index             body: {"contacts": [...]}
                        → 200 {"indexed": N}
                        → přeindexuje celý dataset (ne append)

GET  /search            ?q=<dotaz>
                        → 200 {"results":[...], "total":N, "took_ms":N}
                        → bez q= nebo q="" → results:[], total:0
                        → výsledky seřazeny dle skóre DESC

GET  /health            → 200 {"status":"ok","service":"search-rust"}
GET  /stats             → 200 {"requests":N,"errors":N,"uptime_s":N}
```

**Algoritmus relevance:**
```
tokenize(text) = lowercase(text).split(non-alphanumeric).filter(non-empty)

váhy: last_name=3.0, first_name=2.0, email=1.0, phones=1.0, category=1.0

pro každý query token:
  exact match v indexu    → score += weight
  prefix match v indexu   → score += weight * 0.5

výsledky: sort desc dle score, vrátit Contact objekty (bez score)
```

### gateway-go (:9000)

```
POST   /services        body: {"name":str, "url":str, "health_path":str}
                        → 201 {"status":"registered","name":str}
                        → 400 pokud chybí name nebo url
                        → side effect: přidá edge "gateway-go→name" do topologie

GET    /services        → 200 array ServiceEntry (včetně request_count, error_count)

DELETE /services/{name} → 200 | 404

GET    /health          → 200 {"status":"ok","service":"gateway-go","uptime_s":N}

GET    /stats           → 200 {service_name: {"request_count":N,"error_count":N}}

GET    /topology        → 200 {"edges":[{"from":str,"to":str},...]}

POST   /topology/edge   body: {"from":str, "to":str}
                        → 201

GET    /{service}/{path} → reverse proxy na registrovanou službu
                        → strip /{service} prefix, forward /{path}
                        → 404 pokud service není registrován
                        → 5xx odpověď incrementuje error_count
```

**Health checker:** background goroutine, tick každých 10s, GET `url+health_path`, měří latenci

### bff-python (:8989)

```
GET  /health            → 200 {"status":"ok","service":"bff-python"}

GET  /api/contacts      ?q= → proxy → CONTACTS_URL/contacts?q=
POST /api/contacts           → proxy → CONTACTS_URL/contacts
PUT  /api/contacts/{id}      → proxy → CONTACTS_URL/contacts/{id}
DELETE /api/contacts/{id}    → proxy → CONTACTS_URL/contacts/{id}

GET  /api/search        ?q= → proxy → SEARCH_URL/search?q=

GET  /api/services           → proxy → GATEWAY_URL/services
GET  /api/topology           → proxy → GATEWAY_URL/topology

GET  /api/stats              → parallel GET /stats na contacts-cpp, search-rust, gateway-go
                               → vrátí array tří objektů, timeout 5s na každý
                               → chyba = {"service":name,"error":str}

GET  /                       → index.html (HTML tabulka)
```

---

## Meziservisní komunikace — přesné URL

| Volající | Cíl | URL | Kdy |
|---|---|---|---|
| contacts-cpp | search-rust | `SEARCH_URL/index` | každý POST/PUT kontaktu |
| contacts-cpp | gateway-go | `GATEWAY_URL/services` | startup, max 5 pokusů |
| search-rust | contacts-cpp | `CONTACTS_URL/contacts` | startup, retry loop |
| search-rust | gateway-go | `GATEWAY_URL/services` | startup, po indexaci |
| bff-python | gateway-go | `GATEWAY_URL/services` | startup lifespan |
| bff-python | contacts-cpp | `CONTACTS_URL/contacts[/{id}]` | každý /api/contacts request |
| bff-python | search-rust | `SEARCH_URL/search` | každý /api/search request |
| bff-python | gateway-go | `GATEWAY_URL/services\|topology` | /api/services, /api/topology |
| bff-python | všichni | `{URL}/stats` | /api/stats, paralelně |

**Kritická poznámka:** URL při registraci musí být Docker hostname, ne localhost:
- contacts-cpp registruje: `url: "http://contacts-cpp:8080"`
- search-rust registruje: `url: "http://search-rust:8081"` (z env `SELF_URL`)
- bff-python registruje: `url: "http://bff-python:8989"`

---

## Konfigurace (env proměnné)

| Služba | Proměnná | Default | Popis |
|---|---|---|---|
| contacts-cpp | `GATEWAY_URL` | `http://localhost:9000` | |
| contacts-cpp | `SEARCH_URL` | `http://localhost:8081` | |
| contacts-cpp | `DATABASE_URL` | `postgresql://contacts:contacts@postgres:5432/contacts` | připojení k PostgreSQL |
| search-rust | `GATEWAY_URL` | `http://localhost:9000` | |
| search-rust | `CONTACTS_URL` | `http://localhost:8080` | |
| search-rust | `SELF_URL` | `http://localhost:8081` | **musí být Docker hostname v prod** |
| bff-python | `GATEWAY_URL` | `http://localhost:9000` | |
| bff-python | `CONTACTS_URL` | `http://localhost:8080` | |
| bff-python | `SEARCH_URL` | `http://localhost:8081` | |
| gateway-go | `CONTACTS_URL` | `http://localhost:8080` | pro health check |
| gateway-go | `SEARCH_URL` | `http://localhost:8081` | pro health check |

---

## Docker Compose — pořadí startu a závislosti

```
postgres        → žádná závislost, startuje první (spolu s gateway-go)
gateway-go      → žádná závislost, startuje první (spolu s postgres)
contacts-cpp    → čeká na postgres healthy + gateway-go healthy
search-rust     → čeká na contacts-cpp healthy (potřebuje data pro indexaci)
bff-python      → čeká na gateway-go healthy
```

Health check služeb: `curl -f http://localhost:{port}/health`, interval 10s, retries 5, start_period 10s
Health check postgres: `pg_isready -U contacts`, interval 5s, retries 5, start_period 5s

**Volumes:** `postgres_data` — data přežijí restart kontejnerů; smazána pouze pomocí `docker compose down -v`

**Pozorovaný startup čas:** ~25s od `docker compose up` do všech healthy

---

## Výkonnostní charakteristiky

Naměřeno: 1000 req, 20 concurrent, Docker na localhostu

| Služba | Endpoint | req/s | p50 | p99 | p99.9 |
|---|---|---|---|---|---|
| contacts-cpp | GET /contacts | 1 499 | 3.4 ms | 453 ms | 497 ms |
| search-rust | GET /search | 4 193 | 4.3 ms | 8.9 ms | 11.3 ms |
| gateway-go | GET /health | ~4 000 | 2.3 ms | 4.3 ms | 5.7 ms |
| bff-python | GET /api/stats | 186 | 56 ms | 87 ms | 110 ms |

**Interpretace:**
- contacts-cpp má vysoké p99 — `shared_mutex` pod contention, každá notifikace search-rust otevírá nové TCP spojení (bez connection pool)
- search-rust je nejkonzistentnější — Rust async, žádný GC, `RwLock` efektivní pro read-heavy workload
- bff-python /api/stats = `max(latence tří služeb)` + Python overhead

---

## Technologický stack a verze

| Služba | Jazyk | Framework | Klíčové závislosti |
|---|---|---|---|
| contacts-cpp | C++20 | cpp-httplib v0.14.3 | nlohmann/json v3.11.3, libpqxx, libpq |
| postgres | PostgreSQL 16 | postgres:16-alpine | — |
| search-rust | Rust (edition 2021) | Axum | tokio (full), reqwest, serde |
| gateway-go | Go 1.22 | stdlib only | žádné externí |
| bff-python | Python 3.12 | FastAPI ≥0.111 | httpx ≥0.27, uvicorn |

---

## Testy — co a jak se testuje

### contacts-cpp — vlastní runner bez externích deps
Testuje interní logiku přímo (ne HTTP):
- `ContactStore`: add/get_all/get_by_id/update/remove, sort, filter, thread safety (10 vláken × 100 přidání)
- `generate_uuid()`: délka, pozice pomlček, verze bit, variant bit, unikátnost
- JSON round-trip: `contact_from_json` → `contact_to_json` zachovává vše

### search-rust — cargo test (#[cfg(test)])
- Unit: tokenize, prázdný dotaz, exact match, prefix match, váhy, rebuild
- Handler: přes `tower::ServiceExt::oneshot` (bez síťového volání)

### gateway-go — go test (net/http/httptest)
- Každý test vytvoří izolovaný gateway (`newTestGateway()`)
- Proxy test: `httptest.NewServer` jako fake backend

### bff-python — pytest
- Mockuje `httpx.AsyncClient` přes vlastní `_AsyncMockTransport`
- Testuje všechny proxy routes, stats fan-out, error handling

---

## Známé limitace a gotchas

1. **PostgreSQL persistence** — data přežijí restart contacts-cpp; smazána jsou pouze pomocí `docker compose down -v` (smaže `postgres_data` volume)
2. **Search index není perzistentní** — při restartu search-rust se stáhnou data z contacts-cpp (z DB) a přeindexují
3. **Notifikace search je best-effort** — pokud search-rust není dostupný při zápisu, index zaostane
4. **contacts-cpp p99 latence** — bez HTTP connection pool pro notifikace search
5. **Žádná autentizace** — API je zcela otevřené
6. **bff-python /api/stats latence = max(3 backendy)** — jedna pomalá služba zpomalí celý endpoint

---

## MCP Memory Server

Střední vrstva mezi Claude Code CLI a pamětí projektu. Claude místo pasivního čtení MD souborů aktivně dotazuje strukturovanou databázi.

### Jak to funguje

```
Claude Code CLI
      │  volá tool: memory_search("PostgreSQL port")
      ▼
.claude/mcp_server.py          ← Python subprocess, stdio transport
      │
      ├── .claude/memory.db    ← SQLite + FTS5, přenositelné s projektem
      ├── git log              ← historie změn
      └── ./status.sh          ← živý stav kontejnerů
```

Claude Code spustí MCP server jako subprocess při startu session. Komunikace probíhá přes stdin/stdout (žádný HTTP port).

### Konfigurace (přidat do `.claude/settings.local.json`)

```json
{
  "mcpServers": {
    "polycontacts-memory": {
      "command": "python3",
      "args": [".claude/mcp_server.py"]
    }
  }
}
```

### Nástroje které MCP server vystavuje

**`memory_search(query: str) → list`**
Full-text hledání v SQLite FTS5. Vrátí relevantní záznamy.
```
memory_search("PostgreSQL port") → ["Port 5432 byl obsazený → 5433", ...]
memory_search("bug registrace")  → ["contacts-cpp volal /register místo /services", ...]
```

**`memory_add(category: str, content: str) → ok`**
Uloží nový záznam do databáze. Claude volá automaticky při důležitých zjištěních.
```
memory_add("bug", "search-rust musí nastartovat až po contacts-cpp")
memory_add("decision", "BFF volá backends přímo, gateway jen pro registry")
```

**`project_status() → dict`**
Spustí `./status.sh --json` a vrátí strukturovaný stav kontejnerů, HTTP health, počet kontaktů v DB.

**`recent_changes(days: int = 7) → list`**
Parsuje `git log --since=N.days.ago` a vrátí seznam commitů s diffstaty.

### Implementace `.claude/mcp_server.py`

```python
#!/usr/bin/env python3
"""MCP Memory Server pro polycontacts."""
import sqlite3, subprocess, json, sys
from pathlib import Path
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp import types

DB_PATH = Path(__file__).parent / "memory.db"
PROJECT_ROOT = Path(__file__).parent.parent

def init_db():
    conn = sqlite3.connect(DB_PATH)
    conn.execute("""
        CREATE VIRTUAL TABLE IF NOT EXISTS memories USING fts5(
            category, content, created_at UNINDEXED
        )
    """)
    conn.commit()
    return conn

server = Server("polycontacts-memory")

@server.list_tools()
async def list_tools():
    return [
        types.Tool(name="memory_search",
            description="Hledej v paměti projektu",
            inputSchema={"type":"object","properties":{"query":{"type":"string"}},"required":["query"]}),
        types.Tool(name="memory_add",
            description="Ulož nový záznam do paměti",
            inputSchema={"type":"object","properties":{"category":{"type":"string"},"content":{"type":"string"}},"required":["category","content"]}),
        types.Tool(name="project_status",
            description="Živý stav kontejnerů a služeb",
            inputSchema={"type":"object","properties":{}}),
        types.Tool(name="recent_changes",
            description="Poslední změny v git historii",
            inputSchema={"type":"object","properties":{"days":{"type":"integer","default":7}}}),
    ]

@server.call_tool()
async def call_tool(name: str, arguments: dict):
    conn = init_db()

    if name == "memory_search":
        rows = conn.execute(
            "SELECT category, content FROM memories WHERE memories MATCH ? ORDER BY rank LIMIT 10",
            (arguments["query"],)
        ).fetchall()
        result = [{"category": r[0], "content": r[1]} for r in rows]
        return [types.TextContent(type="text", text=json.dumps(result, ensure_ascii=False))]

    if name == "memory_add":
        from datetime import datetime
        conn.execute("INSERT INTO memories VALUES (?, ?, ?)",
            (arguments["category"], arguments["content"], datetime.now().isoformat()))
        conn.commit()
        return [types.TextContent(type="text", text='{"ok": true}')]

    if name == "project_status":
        try:
            result = subprocess.run(
                ["docker", "compose", "ps", "--format", "json"],
                cwd=PROJECT_ROOT / "services",
                capture_output=True, text=True, timeout=10
            )
            services = [json.loads(l) for l in result.stdout.splitlines() if l.strip()]
            status = [{"service": s.get("Service"), "state": s.get("State"), "health": s.get("Health")} for s in services]
        except Exception as e:
            status = {"error": str(e)}
        return [types.TextContent(type="text", text=json.dumps(status, ensure_ascii=False))]

    if name == "recent_changes":
        days = arguments.get("days", 7)
        result = subprocess.run(
            ["git", "log", f"--since={days}.days.ago", "--oneline", "--stat"],
            cwd=PROJECT_ROOT, capture_output=True, text=True
        )
        return [types.TextContent(type="text", text=result.stdout or "Žádné změny")]

    return [types.TextContent(type="text", text='{"error": "unknown tool"}')]

async def main():
    async with stdio_server() as (r, w):
        await server.run(r, w, server.create_initialization_options())

if __name__ == "__main__":
    import asyncio
    asyncio.run(main())
```

### Instalace závislosti

```bash
pip install mcp
# nebo přidat do pyproject.toml bff-python jako dev závislost
```

### Přenositelnost

```bash
# .gitignore — přidat:
.claude/memory.db      # lokální data, přenáší se přes export.sh

# export.sh automaticky zabalí .claude/ včetně memory.db
# bootstrap.sh nainstaluje pip install mcp
```

### Seed data do memory.db při prvním spuštění

Po postavení projektu spustit jednou:
```
memory_add("architecture", "BFF volá contacts-cpp a search-rust přímo — gateway jen pro /services a /topology")
memory_add("architecture", "search index není perzistentní — při restartu se přeindexuje z PostgreSQL")
memory_add("bug", "contacts-cpp musí registrovat Docker hostname http://contacts-cpp:8080, ne localhost")
memory_add("config", "PostgreSQL na portu 5433 na hostu, 5432 uvnitř Dockeru")
memory_add("config", "search-rust startuje až po contacts-cpp healthy — potřebuje data pro indexaci")
```

---

## Soubory ke vzniku (kompletní seznam)

```
.gitignore
README.md
ARCHITECTURE.md
start.sh                              (bash, kontrola prerekvizit + spuštění)
status.sh                             (bash, live check: kontejnery, HTTP, DB, logy)
export.sh                             (bash, tar.gz snapshot projektu)
bootstrap.sh                          (bash, nastavení na novém stroji)
services/docker-compose.yml           (postgres:16-alpine service + postgres_data volume)
services/contacts-cpp/Dockerfile      (builder: libpqxx-dev libpq-dev; runtime: libpq5)
services/contacts-cpp/CMakeLists.txt  (libpqxx + libpq závislosti)
services/contacts-cpp/src/main.cpp    (PostgreSQL persistence přes libpqxx)
services/contacts-cpp/src/test_contacts.cpp
services/search-rust/Dockerfile
services/search-rust/Cargo.toml
services/search-rust/src/main.rs     (testy inline v #[cfg(test)])
services/gateway-go/Dockerfile
services/gateway-go/go.mod
services/gateway-go/main.go
services/gateway-go/main_test.go
services/bff-python/Dockerfile
services/bff-python/pyproject.toml
services/bff-python/app/main.py
services/bff-python/app/static/index.html
services/bff-python/tests/__init__.py
services/bff-python/tests/test_main.py
.claude/mcp_server.py                 (MCP memory server — Python, SQLite FTS5)
.claude/settings.local.json           (bypassPermissions + hooks + mcpServers)
```
