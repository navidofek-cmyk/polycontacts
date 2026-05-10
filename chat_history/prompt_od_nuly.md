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
1. gateway-go nastartuje první, otevře port 9000
2. contacts-cpp nastartuje, zaregistruje se u gateway, port 8080
3. search-rust nastartuje, stáhne kontakty z contacts-cpp, zaindexuje je, zaregistruje se u gateway, port 8081
4. bff-python nastartuje, zaregistruje se u gateway, port 8989
5. Systém je ready — `GET /health` na všech portech vrací 200

---

## Architektura (white box)

```
[Browser]
    │ HTTP :8989
    ▼
[bff-python]  FastAPI/Python
    │
    ├──────────────────────────────► [contacts-cpp]  C++20  :8080
    │   /api/contacts → /contacts                     CRUD, in-memory
    │   /api/contacts/{id} → /contacts/{id}           shared_mutex, thread pool 8
    │                                │
    │                                │ POST /index (při každém zápisu)
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
gateway-go      → žádná závislost, startuje první
contacts-cpp    → čeká na gateway-go healthy
search-rust     → čeká na contacts-cpp healthy (potřebuje data pro indexaci)
bff-python      → čeká na gateway-go healthy
```

Health check všech: `curl -f http://localhost:{port}/health`, interval 10s, retries 5, start_period 10s

**Pozorovaný startup čas:** ~20s od `docker compose up` do všech healthy

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
| contacts-cpp | C++20 | cpp-httplib v0.14.3 | nlohmann/json v3.11.3 |
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

1. **In-memory only** — restart kontejneru = ztráta dat (seed data se načtou znovu)
2. **Search index není perzistentní** — při restartu search-rust se stáhnou data z contacts-cpp a přeindexují
3. **Notifikace search je best-effort** — pokud search-rust není dostupný při zápisu, index zaostane
4. **contacts-cpp p99 latence** — bez HTTP connection pool pro notifikace search
5. **Žádná autentizace** — API je zcela otevřené
6. **bff-python /api/stats latence = max(3 backendy)** — jedna pomalá služba zpomalí celý endpoint

---

## Soubory ke vzniku (kompletní seznam)

```
.gitignore
README.md
ARCHITECTURE.md
start.sh                              (bash, kontrola prerekvizit + spuštění)
services/docker-compose.yml
services/contacts-cpp/Dockerfile
services/contacts-cpp/CMakeLists.txt
services/contacts-cpp/src/main.cpp
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
```
