# Prompt: Contacts Microservices od nuly

---

Vytvoř kompletní microservices projekt — adresář kontaktů — ve složce `contacts/`.
Projekt musí být funkční, spustitelný přes Docker Compose, s testy a dokumentací.

---

## Architektura

4 nezávislé HTTP služby komunikující přes JSON:

```
Prohlížeč → bff-python:8989 → contacts-cpp:8080
                             → search-rust:8081
                             → gateway-go:9000
contacts-cpp → search-rust (notifikace při změně dat)
všechny služby → gateway-go (registrace při startu)
```

---

## Datový model — Contact

```json
{
  "id":         "uuid-v4",
  "first_name": "Jana",
  "last_name":  "Nováková",
  "email":      "jana@example.com",
  "phones":     [{"label": "mobil", "number": "+420 601 111 222"}],
  "category":   "Friend"
}
```
Povolené kategorie: `Friend`, `Work`, `Family`, `Other`, `Colleague`

---

## Struktura projektu

```
contacts/
├── .gitignore
├── README.md
├── ARCHITECTURE.md
├── start.sh
├── src/                          # standalone C++20 verze (zachovat)
├── api/                          # standalone FastAPI verze (zachovat)
└── services/
    ├── docker-compose.yml
    ├── contacts-cpp/
    │   ├── Dockerfile
    │   ├── CMakeLists.txt
    │   └── src/
    │       ├── main.cpp
    │       └── test_contacts.cpp
    ├── search-rust/
    │   ├── Dockerfile
    │   ├── Cargo.toml
    │   └── src/main.rs
    ├── gateway-go/
    │   ├── Dockerfile
    │   ├── go.mod
    │   ├── main.go
    │   └── main_test.go
    └── bff-python/
        ├── Dockerfile
        ├── pyproject.toml
        └── app/
            ├── main.py
            ├── static/index.html
            └── tests/
                ├── __init__.py
                └── test_main.py
```

---

## 1. contacts-cpp (C++20, port 8080)

**Závislosti:** cpp-httplib v0.14.3, nlohmann/json v3.11.3 (přes CMake FetchContent)

**ContactStore třída:**
- `std::vector<Contact>` + `mutable std::shared_mutex`
- `add(Contact)` — `std::unique_lock`
- `get_all(query="")` — `std::shared_lock`, case-insensitive substring filter přes first_name+last_name+email+category, seřadit dle (last_name, first_name)
- `get_by_id(id)` → `std::optional<Contact>`
- `update(id, Contact)` → bool
- `remove(id)` → bool

**UUID generátor:** RFC 4122 v4 přes `std::mt19937` + `std::uniform_int_distribution<uint32_t>`, formát `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`

**HTTP server:** `httplib::Server` s `ThreadPool(8)`

**API endpointy:**
```
GET  /contacts?q=    → JSON array, sorted
GET  /contacts/{id}  → JSON object nebo 404
POST /contacts       → 201 + objekt, vygenerovat UUID, validovat first_name+last_name
PUT  /contacts/{id}  → 200 nebo 404
DEL  /contacts/{id}  → 204 nebo 404
GET  /health         → {"status":"ok","service":"contacts-cpp","threads":8}
GET  /stats          → {"requests":N,"errors":N,"uptime_s":N}
```

**Atomic čítače:** `std::atomic<uint64_t>` pro req_count a err_count

**Notifikace search:** Po každém POST/PUT spustit `std::thread([...](){ httplib::Client cli(search_url); cli.Post("/index", contact_json, "application/json"); }).detach()`

**Registrace u gateway při startu:**
```cpp
// background thread, 5 pokusů, delay 2s mezi pokusy
POST GATEWAY_URL/services
{"name":"contacts","url":"http://contacts-cpp:8080","health_path":"/health"}
```

**Seed data:** 4 kontakty (Jana Nováková, Petr Svoboda, Marie Horáková, Tomáš Dvořák)

**Env:** `GATEWAY_URL=http://localhost:9000`, `SEARCH_URL=http://localhost:8081`

**CMakeLists.txt:**
- C++20, FetchContent pro httplib a nlohmann_json
- Target `contacts_server` (main.cpp)
- Target `test_contacts` (test_contacts.cpp) — linkuje pouze nlohmann_json + pthread

**Dockerfile:** Ubuntu 24.04 builder (cmake, make, g++, git, libssl-dev), runtime stage s libssl3+curl

---

## 2. search-rust (Rust/Axum, port 8081)

**Cargo.toml závislosti:** axum, tokio (full), serde+serde_json, reqwest (json feature), tracing+tracing-subscriber

**Dev závislosti:** tower, http-body-util

**SearchIndex struct:**
```rust
pub struct SearchIndex {
    pub contacts: HashMap<String, Contact>,
    pub inverted_index: HashMap<String, Vec<(String, f32)>>,
}
```

**Tokenizace:** lowercase + split na non-alphanumeric + filter prázdné

**Váhy při indexování:**
```
last_name:  3.0
first_name: 2.0
email:      1.0
phones:     1.0 (label i number)
category:   1.0
```

**Vyhledávání:**
1. Exact token match → +weight
2. Prefix match (`indexed.starts_with(query_token)`) → +weight * 0.5
3. Agregovat skóre per contact_id, seřadit desc

**AppState:** `Arc<RwLock<SearchIndex>>` + `Arc<AtomicU64>` pro requests/errors + `Instant` start_time

**API:**
```
POST /index       → body: {"contacts":[...]}, vrátí {"indexed":N}
GET  /search?q=   → {"results":[...], "total":N, "took_ms":N}
GET  /health      → {"status":"ok","service":"search-rust"}
GET  /stats       → {"requests":N,"errors":N,"uptime_s":N}
```

**Startup (tokio::spawn background task):**
1. Retry loop: GET CONTACTS_URL/contacts dokud neuspěje (sleep 2s mezi pokusy)
2. POST na vlastní SELF_URL/index
3. POST GATEWAY_URL/services s `{"name":"search-rust","url":SELF_URL,"health":SELF_URL+"/health"}`

**Env:** `GATEWAY_URL`, `CONTACTS_URL`, `SELF_URL=http://localhost:8081`

**Dockerfile:** rust:1.87-slim builder s cache trick (dummy main.rs pro deps), debian:bookworm-slim runtime s curl

---

## 3. gateway-go (Go, port 9000)

**Pouze stdlib** — žádné externí závislosti

**ServiceEntry struct:**
```go
type ServiceEntry struct {
    Name, URL, HealthPath string
    Routes       []string
    Status       string  // "healthy"|"unhealthy"|"unknown"
    LatencyMs    int64
    LastChecked, RegisteredAt time.Time
    RequestCount atomic.Int64  // json:"-"
    ErrorCount   atomic.Int64  // json:"-"
}
```

**TopologyEdge:** `{From, To string}`

**Gateway struct:** `sync.RWMutex` + `map[string]*ServiceEntry` + `map[string]*httputil.ReverseProxy` + edges `[]TopologyEdge` s vlastním `sync.RWMutex`

**API:**
```
POST   /services        → registrovat, vrátit 201, automaticky přidat edge gateway-go→service
GET    /services        → list se stats (atomic counters převést na int64 pro JSON)
DELETE /services/{name} → 200 nebo 404
GET    /health          → {"status":"ok","service":"gateway-go","uptime_s":N}
GET    /stats           → map service→{request_count, error_count}
GET    /topology        → {"edges":[...]}
POST   /topology/edge   → přidat {from,to}, vrátit 201
/*                      → reverse proxy
```

**Proxy logika:**
- Path `/{service}/rest` → strip `/{service}` → forward `/rest`
- `statusRecorder` wrapper pro zachytávání HTTP status kódu
- 5xx → `svc.ErrorCount.Add(1)`

**Health checker:** goroutine s `time.NewTicker(10s)`, pro každou službu spustit goroutinu → GET health URL → měřit latenci → nastavit Status

**HTTP server:** `ReadTimeout:30s, WriteTimeout:60s, IdleTimeout:120s`

**Dockerfile:** golang:1.22-alpine builder, alpine:3.19 runtime s curl, vlastní HEALTHCHECK

---

## 4. bff-python (FastAPI, port 8989)

**pyproject.toml závislosti:** fastapi>=0.111, uvicorn[standard]>=0.29, httpx>=0.27

**Dev závislosti:** pytest>=8.0, pytest-asyncio>=0.23

**Env proměnné:**
```python
GATEWAY_URL  = os.getenv("GATEWAY_URL",  "http://localhost:9000")
CONTACTS_URL = os.getenv("CONTACTS_URL", "http://localhost:8080")
SEARCH_URL   = os.getenv("SEARCH_URL",   "http://localhost:8081")
```

**httpx.AsyncClient** — vytvořit v lifespan, zavřít při ukončení

**Proxy helper `_proxy(request, url, **kwargs)`:**
- Přeposlat method, body, headers (bez host a content-length)
- Vrátit `Response(content, status_code, media_type)`

**API:**
```
GET  /health              → {"status":"ok","service":"bff-python"}
GET  /api/contacts?q=     → proxy → CONTACTS_URL/contacts
POST /api/contacts        → proxy → CONTACTS_URL/contacts
PUT  /api/contacts/{id}   → proxy → CONTACTS_URL/contacts/{id}
DEL  /api/contacts/{id}   → proxy → CONTACTS_URL/contacts/{id}
GET  /api/search?q=       → proxy → SEARCH_URL/search
GET  /api/services        → proxy → GATEWAY_URL/services
GET  /api/topology        → proxy → GATEWAY_URL/topology
GET  /api/stats           → parallel fan-out (viz níže)
/                         → StaticFiles html=True
```

**`/api/stats` — parallel fan-out:**
```python
async def _fetch_stats(name, base_url):
    try:
        r = await _client.get(f"{base_url}/stats", timeout=5.0)
        return {"service": name, **(r.json() if r.status_code==200 else {})}
    except Exception as exc:
        return {"service": name, "error": str(exc)}

targets = [("contacts-cpp", CONTACTS_URL), ("search-rust", SEARCH_URL), ("gateway-go", GATEWAY_URL)]
results = await asyncio.gather(*[_fetch_stats(n, u) for n, u in targets])
```

**Lifespan registrace:**
```python
await _client.post(f"{GATEWAY_URL}/services", json={
    "name": "bff-python",
    "url": "http://bff-python:8989",
    "health_path": "/health"
})
```

**Dockerfile:** python:3.12-slim + apt install curl + pip install uv + uv sync --no-dev, CMD uvicorn na portu 8989

---

## 5. docker-compose.yml

```yaml
services:
  gateway-go:
    ports: ["9000:9000"]
    healthcheck: curl -f http://localhost:9000/health

  contacts-cpp:
    ports: ["8080:8080"]
    env: GATEWAY_URL, SEARCH_URL
    depends_on: gateway-go (healthy)
    healthcheck: curl -f http://localhost:8080/health

  search-rust:
    ports: ["8081:8081"]
    env: GATEWAY_URL, CONTACTS_URL, SELF_URL=http://search-rust:8081
    depends_on: contacts-cpp (healthy)
    healthcheck: curl -f http://localhost:8081/health

  bff-python:
    ports: ["8989:8989"]
    env: GATEWAY_URL, CONTACTS_URL, SEARCH_URL
    depends_on: gateway-go (healthy)
    healthcheck: curl -f http://localhost:8989/health

networks:
  contacts-net: bridge
```

Všechny healthchecky: `interval:10s, timeout:5s, retries:5, start_period:10s`

---

## 6. start.sh

Bash skript který:
1. Zkontroluje: docker nainstalován, daemon běží, docker compose dostupný
2. Zkontroluje volné porty 8989/9000/8080/8081 (ss nebo lsof)
3. Zkontroluje diskové místo (varuje pod 2 GB)
4. Zastaví existující kontejnery (docker compose down --remove-orphans)
5. Spustí build a start (docker compose up --build -d)
6. Počká na healthy stav všech 4 služeb (max 120s, poll každé 3s)
7. Curl ping na každý /health endpoint
8. Vypíše URLs

Barevný výstup (ANSI escape kódy: zelená ✔, žlutá ⚠, červená ✘, cyan pro nadpisy)

---

## 7. Testy

### bff-python — pytest
Mockovat httpx přes vlastní `_AsyncMockTransport`, testovat všechny API endpointy bez síťového volání.

### gateway-go — go test
`newTestGateway()` helper pro izolovaný gateway, `httptest.NewServer` jako fake backend pro proxy test.

### search-rust — cargo test
Unit testy `SearchIndex` (tokenize, exact/prefix match, váhy, rebuild). Handler testy přes `tower::ServiceExt::oneshot`.

### contacts-cpp — vlastní runner
Vlastní assert makra bez externích závislostí. Testovat ContactStore (CRUD, sort, filter, thread safety), UUID, JSON round-trip.

---

## 8. .gitignore

```
.venv/, __pycache__/, uv.lock
build/, *.o
services/search-rust/target/
services/gateway-go/gateway-go
.claude/settings.local.json
```

---

## Poznámky k implementaci

- contacts-cpp notifikuje search přes detached thread — ignorovat chyby
- search-rust registruje se s URL `http://search-rust:8081` (Docker hostname), ne localhost
- bff-python registruje se s `http://bff-python:8989` (ne 8000)
- gateway proxy: `GET /contacts/xyz` → service="contacts", forward path="/xyz"
- BFF volá contacts-cpp a search-rust přímo (CONTACTS_URL/SEARCH_URL), ne přes gateway proxy
- gateway přidá automaticky TopologyEdge `gateway-go→service` při každé registraci
