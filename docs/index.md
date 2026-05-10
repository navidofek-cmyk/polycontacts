# polycontacts

Polycontacts je ukázkový adresář kontaktů rozdělený do čtyř nezávislých microservices, každá napsaná v jiném jazyce. Projekt vznikl jako vzdělávací příklad toho, jak spolu různé technologie komunikují přes HTTP/JSON v reálném Docker prostředí.

## Služby

| Služba | Jazyk | Port | Role |
|---|---|---|---|
| `contacts-cpp` | C++20 | 8080 | Autoritativní CRUD + PostgreSQL persistence |
| `search-rust` | Rust / Axum | 8081 | Fulltextový invertovaný index v paměti |
| `gateway-go` | Go stdlib | 9000 | Service registry, health check, reverse proxy |
| `bff-python` | Python / FastAPI | 8989 | Backend for Frontend — jediný vstupní bod |
| `postgres` | PostgreSQL 16 | 5432 | Relační úložiště pro contacts-cpp |

## Quick start

```bash
git clone https://github.com/navidofek-cmyk/polycontacts
cd polycontacts/services
docker compose up --build -d
# Frontend: http://localhost:8989
```

Po spuštění Docker Compose nastartuje všechny služby ve správném pořadí (postgres → gateway → contacts-cpp → search-rust → bff-python). Seed data (4 kontakty) se vloží automaticky.

## O tomto tutorialu

- **Jak funguje BFF pattern** — proč prohlížeč volá jen jednu adresu, i když za ní běží tři různé backendy
- **Jak Rust, Go a C++ řeší souběžnost jinak** — RwLock bez GC, goroutiny, shared_mutex + condition_variable
- **Proč se výkonnostní charakteristiky tak liší** — od 186 req/s (Python fan-out) po 4193 req/s (Rust in-memory index)
