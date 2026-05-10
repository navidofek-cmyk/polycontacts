# polycontacts — Claude Code instrukce

## Co je tento projekt
Microservices adresář kontaktů. 5 služeb v různých jazycích, Docker Compose, PostgreSQL.

## Jak spustit
```bash
./start.sh          # kontrola prerekvizit + build + start
cd services && docker compose down   # stop
```
Frontend: http://localhost:8989

## Architektura (zkratka)
- **bff-python** :8989 — jediný vstupní bod, FastAPI proxy
- **contacts-cpp** :8080 — C++20, CRUD, PostgreSQL (libpqxx)
- **search-rust** :8081 — Rust/Axum, invertovaný index
- **gateway-go** :9000 — Go, service registry, health check
- **postgres** :5433 (host) — PostgreSQL 16, volume `postgres_data`

## Kde jsou věci
- Služby: `services/{contacts-cpp,search-rust,gateway-go,bff-python}/`
- Docker Compose: `services/docker-compose.yml`
- Testy: v každé službě (pytest, go test, cargo test, vlastní C++ runner)
- Dokumentace: `README.md`, `ARCHITECTURE.md`, `chat_history/prompt_od_nuly.md`

## Jak testovat
```bash
# Python
cd services/bff-python && uv run pytest tests/ -v

# Go
cd services/gateway-go && docker run --rm -v $(pwd):/src -w /src golang:1.22 go test -v ./...

# Rust
cd services/search-rust && cargo test

# C++
cd services/contacts-cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel $(nproc) && ./build/test_contacts
```

## Pravidla pro práci
- Nikdy nerozbíjet běžící kontejnery bez nutnosti
- Po změně contacts-cpp nebo bff-python: `docker compose up --build -d <service>`
- Po změně HTML: stačí `docker compose restart bff-python`
- Commit až když testy projdou
- Databáze: `contacts:contacts@localhost:5433/contacts` (z hostitele)
