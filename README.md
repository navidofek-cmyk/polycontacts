# polycontacts — Microservices projekt

**📖 [Dokumentace a code walkthrough tutorial](https://navidofek-cmyk.github.io/polycontacts)**

Ukázkový projekt demonstrující microservices architekturu v několika jazycích.

## Architektura

```
Browser
   │
   ▼
bff-python (FastAPI) :8989        ← jediný vstupní bod
   │         │
   │         ├── /api/contacts ──► contacts-cpp (C++20) :8080
   │         ├── /api/search   ──► search-rust (Rust/Axum) :8081
   │         ├── /api/services ──► gateway-go (Go) :9000
   │         └── /api/topology ──► gateway-go (Go) :9000
   │
   └── /api/stats (parallel fan-out na všechny služby)

contacts-cpp ──► postgres :5432   ← persistentní úložiště kontaktů
```

## Služby

| Služba | Jazyk | Port | Popis |
|---|---|---|---|
| `bff-python` | Python / FastAPI | 8989 | Frontend + API proxy |
| `contacts-cpp` | C++20 / httplib | 8080 | CRUD kontaktů, PostgreSQL store |
| `search-rust` | Rust / Axum | 8081 | Full-text search, invertovaný index |
| `gateway-go` | Go | 9000 | Service registry, health check, topology |
| `postgres` | PostgreSQL 16 | 5432 | Persistentní úložiště kontaktů |

## Struktura projektu

```
contacts/
├── start.sh                          # Spouštěcí skript (kontrola + build + start)
│
├── src/                              # Původní standalone C++20 verze
│   ├── main.cpp
│   ├── contact_book.cpp
│   └── contact_book.hpp
│
├── api/                              # Původní standalone FastAPI verze
│   ├── main.py
│   └── static/index.html
│
├── services/                         # Microservices
│   ├── docker-compose.yml
│   │
│   ├── contacts-cpp/                 # C++20 CRUD service
│   │   ├── Dockerfile
│   │   ├── CMakeLists.txt
│   │   └── src/main.cpp
│   │
│   ├── search-rust/                  # Rust full-text search
│   │   ├── Dockerfile
│   │   ├── Cargo.toml
│   │   └── src/main.rs
│   │
│   ├── gateway-go/                   # Go API gateway
│   │   ├── Dockerfile
│   │   ├── go.mod
│   │   └── main.go
│   │
│   └── bff-python/                   # Python BFF + frontend
│       ├── Dockerfile
│       ├── pyproject.toml
│       └── app/
│           ├── main.py
│           └── static/index.html
│
└── chat_history/                     # Historie konverzací s Claude
    └── 2026-05-10.md
```

## Spuštění

```bash
./start.sh
```

Skript provede:
1. Kontrolu prostředí (Docker, volné porty, diskové místo)
2. Build všech Docker obrazů
3. Start kontejnerů (`docker compose up -d`)
4. Čekání na health checks (max 120 s)
5. Ověření živosti všech endpointů

**Frontend:** http://localhost:8989

### Ruční spuštění

```bash
cd services
docker compose up --build -d    # start
docker compose down             # stop (data zůstanou v postgres_data volume)
docker compose down -v          # stop + smazání dat (smaže volume)
docker compose logs -f          # logy
```

## API endpointy (přes BFF)

| Metoda | Endpoint | Popis |
|---|---|---|
| `GET` | `/api/contacts?q=` | Seznam kontaktů (volitelně filtrovaný) |
| `POST` | `/api/contacts` | Přidat kontakt |
| `PUT` | `/api/contacts/{id}` | Upravit kontakt |
| `DELETE` | `/api/contacts/{id}` | Smazat kontakt |
| `GET` | `/api/search?q=` | Full-text vyhledávání (Rust) |
| `GET` | `/api/stats` | Statistiky všech služeb (parallel fan-out) |
| `GET` | `/api/services` | Registrované služby v gateway |
| `GET` | `/api/topology` | Graf závislostí mezi službami |

## Technické detaily

### contacts-cpp (C++20)
- `std::shared_mutex` — thread-safe CRUD
- `std::ranges::sort`, `std::optional`, `std::format`
- UUID generátor (RFC 4122 variant)
- Thread pool (8 vláken) via httplib
- Notifikuje search-rust při každé změně
- Persistuje data do PostgreSQL (libpqxx), schema se vytvoří automaticky při startu
- Seed data se vloží automaticky pokud je tabulka prázdná

### search-rust (Rust)
- Invertovaný index s váhami: příjmení (3×), jméno (2×), email (1×)
- Prefix matching s 0.5× penalizací
- Tokio async runtime, `Arc<RwLock<SearchIndex>>`
- Při startu načte kontakty z contacts-cpp a indexuje je

### gateway-go (Go)
- Service registry (POST /services)
- Background health checker každých 10 s
- Name-based reverse proxy (`/{service-name}/path`)
- Atomic request/error counters
- Topology graf hran (POST /topology/edge)

### bff-python (FastAPI)
- httpx async client
- Parallel fan-out `/api/stats` pomocí `asyncio.gather`
- Registruje se v gateway při startu
- Statický frontend (HTML tabulka)
