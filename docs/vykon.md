# Výkon & testy

## Naměřené hodnoty

Load test: 1 000 požadavků, 20 souběžných spojení, lokální Docker Compose.

| Služba | req/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| contacts-cpp `GET /contacts` | 1 499 | 3.4 ms | 453 ms | 497 ms |
| search-rust `GET /search` | 4 193 | 4.3 ms | 8.9 ms | 11.3 ms |
| gateway-go `GET /health` | ~4 000 | 2.3 ms | 4.3 ms | 5.7 ms |
| bff-python `GET /api/stats` | 186 | 56 ms | 87 ms | 110 ms |

## Proč contacts-cpp má vysoké p99

P99 je 453 ms — 133× více než p50. Příčiny jsou dvě:

**Lock contention na `shared_mutex`.** HTTP server běží na 8 vláknech. Při zápisu (POST/PUT) čeká `unique_lock` na dokončení všech aktivních čtení. Při mix workloadu se vlákna navzájem blokují a fronta čekajících požadavků roste. Tím se latence občasně vyšplhá ke stovkám milisekund.

**Bez connection poolu pro notifikace search-rust.** Po každém POST/PUT se spustí detached thread, který otevře nové TCP spojení na search-rust. Toto spojení soutěží se stejnou síťovou vrstvou jako databázová připojení. Výsledkem jsou příležitostné spike latence zejména při vyšší zátěži.

## Proč search-rust je konzistentní

P99.9 je jen 2.6× p50 — výjimečná konzistentnost. Důvod je přímý: **žádný garbage collector**. V Rustu se paměť uvolňuje deterministicky při zániku hodnot (RAII), nikdy nedochází k GC pauzám, které v JVM nebo Pythonu mohou trvat desítky až stovky milisekund. `tokio::sync::RwLock` navíc neblokuje OS vlákno, jen pozastaví async task — ostatní vyhledávání pokračují.

## Testy

| Služba | Framework | Počet testů |
|---|---|---|
| contacts-cpp | Vlastní test runner | 18 testů, 46 assertions |
| search-rust | `cargo test` + `tower::ServiceExt::oneshot` | 9 testů |
| gateway-go | `go test` + `net/http/httptest` | 10 testů |
| bff-python | pytest + vlastní httpx transport mock | 16 testů |

Spuštění jednotlivých sad:

```bash
# contacts-cpp
cd services/contacts-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel $(nproc)
./build/test_contacts

# search-rust
cd services/search-rust
cargo test

# gateway-go
cd services/gateway-go
go test -v ./...

# bff-python
cd services/bff-python
uv run pytest tests/ -v
```

Testy jsou izolované — žádný z nich nepotřebuje běžící Docker Compose ani síťová volání.
