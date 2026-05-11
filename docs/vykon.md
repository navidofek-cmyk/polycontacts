# Výkon & testy

## Jak číst čísla výkonu

### p50, p99, p99.9 — proč nestačí průměr

Průměrná latence říká málo. Pokud 99 requestů trvá 1 ms a jeden trvá 1 000 ms, průměr vyjde ~11 ms — číslo, které neodpovídá ani rychlým, ani pomalému requestu.

**Percentily** popisují skutečné chování:

- **p50 (medián)** — polovina requestů je rychlejší, polovina pomalejší. Typický případ.
- **p99** — 99 % requestů je rychlejších. Jeden ze sta uživatelů zažije tuto latenci nebo horší.
- **p99.9** — jeden z tisíce. Důležité při vysokém trafficu: při 1 000 req/s nastane p99.9 každou sekundu.

Pro uživatelský zážitek je p99 kritičtější než průměr. Aplikace která má p50 = 5 ms ale p99 = 2 000 ms bude působit jako pomalá, přestože "průměrně" je rychlá.

### Littleův zákon

```
Throughput = Souběžnost / Latence
```

Při fixní souběžnosti (20 spojení) platí: čím vyšší latence, tím nižší throughput. Proto contacts-cpp s p50 = 3.4 ms dosahuje 1 499 req/s, zatímco bff-python s p50 = 56 ms dosáhne jen 186 req/s — přestože bff-python jen přeposílá requesty a sám moc práce nedělá.

## Naměřené hodnoty

Load test: 1 000 požadavků, 20 souběžných spojení, lokální Docker Compose.

| Služba | req/s | p50 | p99 | p99.9 |
|---|---|---|---|---|
| contacts-cpp `GET /contacts` | 1 499 | 3.4 ms | 453 ms | 497 ms |
| search-rust `GET /search` | 4 193 | 4.3 ms | 8.9 ms | 11.3 ms |
| gateway-go `GET /health` | ~4 000 | 2.3 ms | 4.3 ms | 5.7 ms |
| bff-python `GET /api/stats` | 186 | 56 ms | 87 ms | 110 ms |

## Souběžnost — čtyři různé modely

Každá služba řeší souběžnost jinak. To přímo ovlivňuje výkonnostní profil.

### C++ — vlákna a `shared_mutex`

contacts-cpp spustí **8 OS vláken** (thread pool). Čtení (`shared_lock`) může probíhat paralelně — více vláken čte současně. Zápis (`unique_lock`) blokuje vše: čeká na dokončení všech čtení a pak získá exkluzivní přístup.

Pod zátěží (mix čtení a zápisů) se vlákna navzájem blokují. Výsledek: nízká průměrná latence, ale velké spike při contention → vysoké p99.

### Rust — async + `RwLock` bez GC

search-rust používá **Tokio async runtime** — jeden (nebo více) OS vláken obsluhuje tisíce async tasků. `RwLock` neblokuje OS vlákno, jen pozastaví async task — ostatní tasky pokračují.

Klíčová výhoda: **žádný garbage collector**. V Rustu se paměť uvolňuje deterministicky při zániku hodnot (RAII principle). GC pauzy v JVM nebo Pythonu mohou trvat desítky až stovky milisekund a způsobit nepředvídatelné latency spike. Rust je imunní.

### Go — goroutiny a runtime scheduler

gateway-go používá **goroutiny** — lehké zelené thready spravované Go runtime, ne OS. Spuštění goroutiny stojí ~2 KB paměti a mikrosekundy. OS vlákno stojí ~1 MB a mikrosekundy navíc na context switch.

Go runtime mapuje tisíce goroutiny na desítky OS vláken (M:N threading). Výsledek: výborná souběžnost při nízkém overhead — proto gateway dosahuje ~4 000 req/s i na jednoduchém health check endpointu.

### Python — asyncio event loop

bff-python používá **asyncio** — single-threaded event loop. Jeden thread obsluhuje vše, ale při čekání na síťové I/O (await httpx.get(...)) předá řízení jinému tasku.

Limitace: **GIL** (Global Interpreter Lock) zabraňuje skutečnému paralelismu v Python threadech. Asyncio to obchází tím, že vůbec nepotřebuje thready pro I/O-bound práci. Pro CPU-bound operace by Python byl výrazně pomalejší.

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
