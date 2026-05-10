# Architektura a specifikace — Contacts Microservices

Tento dokument popisuje systém tak, aby ho bylo možné napsat od nuly bez přístupu ke kódu.

---

## 1. Přehled systému

Systém je adresář kontaktů rozdělený do 4 nezávislých HTTP služeb. Každá je napsána v jiném jazyce a zodpovídá za jinou část logiky. Komunikace mezi službami probíhá výhradně přes HTTP/JSON.

```
Prohlížeč
    │  HTTP :8989
    ▼
bff-python          ← jediný vstupní bod, slouží HTML + přeposílá API volání
    ├── /api/contacts  →  contacts-cpp :8080   (CRUD)
    ├── /api/search    →  search-rust  :8081   (full-text)
    ├── /api/services  →  gateway-go   :9000   (registry)
    ├── /api/topology  →  gateway-go   :9000
    └── /api/stats     →  všechny 3 najednou (asyncio.gather)

contacts-cpp  →  search-rust  (notifikace při každé změně dat)
contacts-cpp  →  postgres :5432  (persistentní úložiště)
všechny služby → gateway-go   (registrace při startu)
```

---

## 2. Datový model

### Contact
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
Kategorie: `Friend`, `Work`, `Family`, `Other`, `Colleague`

---

## 3. Služby

### 3.1 contacts-cpp (C++20, port 8080)

**Účel:** Autoritativní zdroj dat o kontaktech. CRUD operace, thread-safe store s PostgreSQL persistencí.

**Technologie:** cpp-httplib (HTTP server), nlohmann/json, libpqxx, C++20

**API:**
```
GET  /contacts?q=    → list kontaktů, volitelně filtrovaný (case-insensitive substring)
                       seřazený dle (last_name, first_name)
GET  /contacts/{id}  → jeden kontakt nebo 404
POST /contacts       → přidat kontakt, vrátí 201 + objekt s vygenerovaným id
PUT  /contacts/{id}  → přepsat kontakt, vrátí 200 nebo 404
DEL  /contacts/{id}  → smazat, vrátí 204 nebo 404
GET  /health         → {"status":"ok","service":"contacts-cpp","threads":8}
GET  /stats          → {"requests":N,"errors":N,"uptime_s":N}
```

**Implementační detaily:**
- `ContactStore` třída s `mutable std::shared_mutex`
- Čtení: `std::shared_lock` (více čtenářů zároveň)
- Zápis: `std::unique_lock` (exkluzivní)
- Thread pool: 8 vláken (`httplib::ThreadPool`)
- UUID: vlastní RFC 4122 v4 generátor přes `std::mt19937`
- Při každém POST/PUT spustí detached thread který POSTuje na `SEARCH_URL/index` (best-effort, ignoruje chybu)
- Při startu: spustí background thread který se pokusí 5× zaregistrovat u gateway (`POST GATEWAY_URL/services`)
- Persistuje data do PostgreSQL přes libpqxx; schema se vytvoří automaticky při startu
- Seed data: 4 kontakty se vloží automaticky pokud je tabulka prázdná

**SQL schema (vytváří se automaticky při startu):**
```sql
CREATE TABLE IF NOT EXISTS contacts (
    id TEXT PRIMARY KEY,
    first_name TEXT NOT NULL,
    last_name TEXT NOT NULL,
    email TEXT,
    category TEXT NOT NULL DEFAULT 'Other'
);
CREATE TABLE IF NOT EXISTS phone_numbers (
    id BIGSERIAL PRIMARY KEY,
    contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,
    label TEXT NOT NULL,
    number TEXT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_contacts_sort ON contacts(last_name, first_name);
```

**Env proměnné:**
```
GATEWAY_URL=http://gateway-go:9000
SEARCH_URL=http://search-rust:8081
DATABASE_URL=postgresql://contacts:contacts@postgres:5432/contacts
```

**Build:** CMake 3.20+, C++20, FetchContent pro httplib a nlohmann/json, systémové balíčky `libpqxx-dev libpq-dev` (builder), `libpq5` (runtime)

---

### 3.2 search-rust (Rust/Axum, port 8081)

**Účel:** Full-text vyhledávání s váženým invertovaným indexem.

**Technologie:** Axum (HTTP), Tokio (async), reqwest (HTTP client), serde

**API:**
```
GET  /search?q=   → {"results":[...], "total":N, "took_ms":N}
POST /index       → {"contacts":[...]}  — přeindexuje vše, vrátí {"indexed":N}
GET  /health      → {"status":"ok","service":"search-rust"}
GET  /stats       → {"requests":N,"errors":N,"uptime_s":N}
```

**Algoritmus indexování:**
```
Váhy tokenů:
  last_name:  3.0
  first_name: 2.0
  email:      1.0
  phones:     1.0
  category:   1.0

Tokenizace: lowercase + split na non-alphanumeric znaky

Invertovaný index: HashMap<token, Vec<(contact_id, weight)>>
Při vyhledávání:
  1. Exact match: +weight
  2. Prefix match (indexed_token.starts_with(query_token)): +weight * 0.5
  3. Seřadit výsledky dle skóre desc
```

**Stav aplikace:** `Arc<RwLock<SearchIndex>>` + atomické čítače requestů/errorů

**Startup sekvence:**
1. Spawn background task
2. Retry loop: fetch `GET CONTACTS_URL/contacts` dokud neuspěje
3. POST na vlastní `/index` s načtenými kontakty
4. Registrace u gateway (`POST GATEWAY_URL/services` s `name:"search-rust"`, `url:SELF_URL`)

**Env proměnné:**
```
GATEWAY_URL=http://gateway-go:9000
CONTACTS_URL=http://contacts-cpp:8080
SELF_URL=http://search-rust:8081
```

---

### 3.3 gateway-go (Go, port 9000)

**Účel:** Service registry, health monitoring, reverse proxy, topology graf.

**Technologie:** stdlib only (`net/http`, `net/http/httputil`, `sync`, `atomic`)

**API:**
```
POST   /services          → registrovat službu {name, url, health_path}
GET    /services          → list všech registrovaných služeb se stats
DELETE /services/{name}   → odregistrovat
GET    /health            → {"status":"ok","service":"gateway-go","uptime_s":N}
GET    /stats             → {service_name: {request_count, error_count}}
GET    /topology          → {"edges":[{"from":"A","to":"B"},...]}
POST   /topology/edge     → přidat hranu {from, to}
/*                        → reverse proxy: /{service-name}/zbytek/cesty
```

**ServiceEntry struct:**
```go
Name, URL, HealthPath string
Routes []string
Status string          // "healthy"|"unhealthy"|"unknown"
LatencyMs int64
LastChecked, RegisteredAt time.Time
RequestCount, ErrorCount atomic.Int64
```

**Proxy logika:**
- URL path: `/{service-name}/rest` → strip `/{service-name}` → forward `/rest` na `service.URL`
- `statusRecorder` wrappuje ResponseWriter pro zachytávání status kódu
- 5xx odpověď → inkrementuje `ErrorCount`

**Health checker:**
- Background goroutine, ticker každých 10s
- Pro každou registrovanou službu: spustí goroutinu → GET `service.URL + service.HealthPath`
- Měří latenci, nastaví Status na "healthy"/"unhealthy"

**Při registraci:** automaticky přidá TopologyEdge `gateway-go → service.Name`

**Synchronizace:** `sync.RWMutex` pro services map, `sync.RWMutex` pro edges slice

---

### 3.4 bff-python (FastAPI/Python, port 8989)

**Účel:** Backend-for-frontend. Servuje HTML stránku, přeposílá API volání na správné backend služby.

**Technologie:** FastAPI, httpx (async), uvicorn

**API:**
```
GET  /health            → {"status":"ok","service":"bff-python"}
GET  /api/contacts?q=   → proxy → CONTACTS_URL/contacts
POST /api/contacts      → proxy → CONTACTS_URL/contacts
PUT  /api/contacts/{id} → proxy → CONTACTS_URL/contacts/{id}
DEL  /api/contacts/{id} → proxy → CONTACTS_URL/contacts/{id}
GET  /api/search?q=     → proxy → SEARCH_URL/search
GET  /api/services      → proxy → GATEWAY_URL/services
GET  /api/topology      → proxy → GATEWAY_URL/topology
GET  /api/stats         → parallel fan-out (viz níže)
GET  /                  → index.html
```

**`/api/stats` — parallel fan-out:**
```python
targets = [
    ("contacts-cpp", CONTACTS_URL),
    ("search-rust",  SEARCH_URL),
    ("gateway-go",   GATEWAY_URL),
]
results = await asyncio.gather(*[fetch_stats(name, url) for name, url in targets])
# každý volá GET {base_url}/stats s timeout=5s
# při chybě vrátí {"service": name, "error": str(exc)}
```

**Proxy helper `_proxy(request, url)`:**
- Přeposílá method, body, headers (bez host a content-length)
- Vrátí Response s upstream status kódem a content-type

**Lifespan:**
- Vytvoří `httpx.AsyncClient(timeout=10.0)` při startu
- Zaregistruje se u gateway: `POST GATEWAY_URL/services` s `{name:"bff-python", url:"http://bff-python:8989", health_path:"/health"}`
- Zavře klienta při ukončení

**Env proměnné:**
```
GATEWAY_URL=http://gateway-go:9000
CONTACTS_URL=http://contacts-cpp:8080
SEARCH_URL=http://search-rust:8081
```

---

## 4. Docker Compose

**Závislosti a pořadí startu:**
```
postgres            (žádná závislost, startuje první)
    ↑ healthy
gateway-go          (žádná závislost na postgres, startuje paralelně)
    ↑ healthy        ↑ healthy
contacts-cpp        (depends_on: postgres healthy + gateway-go healthy)
    ↑ healthy
search-rust         (depends_on: contacts-cpp healthy)
bff-python          (depends_on: gateway-go healthy)
```

**Síť:** `contacts-net` (bridge), služby se oslovují jménem (`contacts-cpp`, `search-rust`, `postgres`, atd.)

**Health checks:** `curl -f http://localhost:{port}/health`, interval 10s, retries 5, start_period 10s
PostgreSQL health check: `pg_isready -U contacts`

**Volumes:**
```
postgres_data       ← persistentní data PostgreSQL (přežijí restart kontejnerů)
```

**Porty (host:container):**
```
8989:8989  bff-python
9000:9000  gateway-go
8080:8080  contacts-cpp
8081:8081  search-rust
5432:5432  postgres
```

**Poznámka k persistenci:** Data přežijí restart jakéhokoliv kontejneru. Pouze `docker compose down -v` smaže volume a tím i data.

---

## 5. Výkon (load test, 1000 req, 20 concurrent)

| Služba | req/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| contacts-cpp GET /contacts | 1 499 | 3.4 ms | 453 ms | 497 ms |
| search-rust GET /search | 4 193 | 4.3 ms | 8.9 ms | 11.3 ms |
| gateway-go GET /health | ~4 000 | 2.3 ms | 4.3 ms | 5.7 ms |
| bff-python GET /api/stats | 186 | 56.5 ms | 87 ms | 110 ms |

**Poznámky:**
- contacts-cpp má vysoké p99 kvůli občasné lock contention na `shared_mutex` při mixu čtení/zápisu
- search-rust je nejkonzistentnější — Rust async + RwLock bez GC pauz
- bff-python /api/stats je nejpomalejší — čeká na 3 paralelní HTTP volání, latence = max(3 services)

---

## 6. Testy

### bff-python — pytest (16 testů)
```bash
cd services/bff-python
uv run pytest tests/ -v
```
Mockuje httpx.AsyncClient přes vlastní transport — žádné síťové volání.

### gateway-go — go test (10 testů)
```bash
cd services/gateway-go
go test -v ./...
# nebo přes Docker:
docker run --rm -v $(pwd):/src -w /src golang:1.22 go test -v ./...
```
Používá `net/http/httptest` — izolovaný gateway bez sdíleného stavu.

### search-rust — cargo test (9 testů)
```bash
cd services/search-rust
cargo test
```
Unit testy SearchIndex + Axum handler testy přes `tower::ServiceExt::oneshot`.

### contacts-cpp — vlastní test runner (18 testů, 46 assertions)
```bash
cd services/contacts-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel $(nproc)
./build/test_contacts
```
Testuje ContactStore, UUID generátor, JSON round-trip, thread safety.

---

## 7. Jak přidat novou službu

1. Vytvoř adresář `services/nova-sluzba/`
2. Implementuj `/health` endpoint
3. Při startu zavolej `POST gateway-go:9000/services` s `{name, url, health_path}`
4. Přidej do `docker-compose.yml` s health checkem
5. Přidej proxy route do `bff-python/app/main.py`
6. Napiš testy

---

## 8. Známé limitace

- **Žádná autentizace** — API je zcela otevřené
- **Search index se nesynchronizuje při startu bff-python** — pouze contacts-cpp notifikuje search při změnách, ne při startu
- **contacts-cpp p99 latence** — bez connection poolu pro HTTP klienty, každá notifikace search-rust otevírá nové spojení
- **Ztráta dat pouze při `docker compose down -v`** — `docker compose down` bez `-v` data zachová (postgres_data volume zůstane)
