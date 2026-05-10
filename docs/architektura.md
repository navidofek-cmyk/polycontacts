# Architektura

## Tok dat

```
Prohlížeč
    │  HTTP :8989
    ▼
bff-python          ← jediný vstupní bod; servuje HTML a přeposílá API volání
    ├── /api/contacts  ──►  contacts-cpp :8080   (CRUD)
    ├── /api/search    ──►  search-rust  :8081   (fulltextový index)
    ├── /api/services  ──►  gateway-go   :9000   (registry)
    ├── /api/topology  ──►  gateway-go   :9000
    └── /api/stats     ──►  všechny 3 najednou (asyncio.gather)

contacts-cpp  ──►  search-rust  (POST /index při každé změně dat)
contacts-cpp  ──►  postgres :5432  (persistentní úložiště)

všechny služby  ──►  gateway-go  (registrace při startu)
```

## Co gateway NENÍ

Gateway-go **není na datové cestě** pro čtení a zápis kontaktů. BFF volá `contacts-cpp` a `search-rust` přímo — gateway by přidala zbytečný network hop a latenci.

Gateway slouží jako **service registry**: ví, které služby běží, na jakých adresách a jestli jsou zdravé. Prohlížeč se může zeptat přes `/api/services` a vidět live stav celého systému. Reverse proxy v gateway existuje jako bonus — umožňuje volat backend přímo přes gateway URL (`/contacts-cpp/contacts`) bez znalosti interní adresy. V produkci to BFF nepoužívá.

## Startup sekvence

Docker Compose spouští služby v závislostním pořadí definovaném `depends_on`:

1. **postgres** nastartuje první, `pg_isready` hlídá připravenost
2. **gateway-go** nastartuje paralelně s postgres (nepotřebuje DB)
3. **contacts-cpp** čeká na `postgres healthy` + `gateway-go healthy`, pak se připojí k DB, vytvoří schema, vloží seed data a v detached vlákně se zaregistruje u gateway
4. **search-rust** čeká na `contacts-cpp healthy`, pak v background tasku načte kontakty přes `GET /contacts`, zaindexuje je a zaregistruje se u gateway
5. **bff-python** čeká na `gateway-go healthy`, při startu vytvoří sdílený `httpx.AsyncClient` a zaregistruje se u gateway

## Meziservisní komunikace

| Volající | Cíl | Endpoint | Kdy |
|---|---|---|---|
| `contacts-cpp` | `postgres` | libpqxx / TCP 5432 | Každý CRUD požadavek |
| `contacts-cpp` | `search-rust` | `POST /index` | Po každém POST/PUT kontaktu (detached thread, best-effort) |
| `contacts-cpp` | `gateway-go` | `POST /services` | Při startu (retry loop, 5 pokusů) |
| `search-rust` | `contacts-cpp` | `GET /contacts` | Při startu (retry loop do úspěchu) |
| `search-rust` | `gateway-go` | `POST /services` | Při startu, jednorázově |
| `bff-python` | `contacts-cpp` | `GET/POST/PUT/DELETE /contacts` | Na každý požadavek z prohlížeče |
| `bff-python` | `search-rust` | `GET /search` | Na každý vyhledávací požadavek |
| `bff-python` | `gateway-go` | `GET /services`, `GET /topology` | Na vyžádání z UI |
| `bff-python` | všechny 3 | `GET /stats` | Paralelně při `/api/stats` |
