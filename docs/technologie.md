# Technologický stack

Přehled všeho co bylo použito v projektu — každá knihovna, framework, nástroj — s vysvětlením proč právě ona a co by bylo alternativou.

---

## C++ — contacts-cpp

### cpp-httplib
**Co je:** Header-only HTTP/1.1 server a klient. Celý kód je v jediném souboru `httplib.h` — žádné linkování, stačí `#include`.

**Proč:** Nulová konfigurační overhead. Alternativy jako Boost.Beast nebo Poco jsou výkonné ale vyžadují složité buildovací nastavení. cpp-httplib funguje out-of-box s CMake za 3 řádky.

**Co umí:** Thread pool (výchozí 8 vláken), regex routes (`svr.Get("/contacts/(.*)", handler)`), chunked responses, HTTPS přes OpenSSL.

**Alternativy:** Boost.Beast (nízkoúrovňové, velmi výkonné), Crow (podobný Flask), Drogon (async, velmi rychlý).

---

### nlohmann/json
**Co je:** Header-only JSON knihovna pro C++. Umožňuje přirozený zápis:
```cpp
json j = {{"id", "123"}, {"name", "Jana"}};
std::string s = j.dump();
```

**Proč:** Nejpoužívanější C++ JSON knihovna. Intuitivní API, dobrá chybová hlášení, single-header (jako httplib — bez linkování). Výkon není její silnou stránkou (simdjson je 10× rychlejší), ale pro tento projekt je dostatečná.

**Alternativy:** simdjson (SIMD instrukce, extrémně rychlý parse), RapidJSON (rychlý, ale méně přívětivé API), Boost.JSON.

---

### libpqxx + libpq
**Co je:** `libpq` je oficiální C klientská knihovna pro PostgreSQL. `libpqxx` je C++ wrapper nad ní — přidává RAII transakce, výjimky a typově bezpečné čtení sloupců.

**Proč:** Jediná oficiálně podporovaná C++ knihovna pro PostgreSQL. RAII transakce zabrání resource leakům — `pqxx::work` automaticky provede ROLLBACK pokud destruktor zavolá před `commit()`.

```cpp
pqxx::work txn(conn);
txn.exec_params("INSERT INTO contacts VALUES ($1, $2)", id, name);
txn.commit();  // bez commit() → automatický ROLLBACK v destruktoru
```

**Alternativy:** Přímé `libpq` (C API, verbose), SOCI, ODB (ORM — zbytečná komplexita pro tento projekt).

---

### CMake
**Co je:** Build systém generátor — nepřekládá kód přímo, ale generuje Makefile nebo Ninja soubory pro konkrétní platformu.

**Proč:** Standard v C++ ekosystému. Správa závislostí přes `FetchContent` stáhne `nlohmann/json` a `cpp-httplib` při prvním buildu automaticky — žádný manuální download.

```cmake
FetchContent_Declare(json URL https://github.com/.../json.hpp)
FetchContent_MakeAvailable(json)
target_link_libraries(contacts nlohmann_json pqxx pq)
```

**Alternativy:** Meson (modernější, rychlejší), Bazel (Google, pro monorepa), xmake.

---

## Rust — search-rust

### Axum
**Co je:** Webový framework postavený nad `tokio` a `hyper`. Routing, extrakce parametrů, middleware přes `tower`.

**Proč:** Nejpřirozenější integrace s `tokio` ekosystémem. Typově bezpečná extrakce parametrů — pokud handler očekává `Json<Contact>` a tělo requestu není validní JSON, Axum vrátí 422 automaticky bez jediného řádku validačního kódu.

```rust
async fn handle_search(
    State(state): State<AppState>,    // dependency injection
    Query(params): Query<SearchParams>, // query string → struct
) -> Result<Json<Value>, StatusCode> { ... }
```

**Alternativy:** Actix-web (nejrychlejší v benchmarcích, ale složitější lifetime constraints), Warp (funkcionální styl), Rocket (ergonomické, ale pomalejší start kompilace).

---

### Tokio
**Co je:** Asynchronní runtime pro Rust. Poskytuje executor (spouštěč tasků), async I/O (síť, soubory), synchronizační primitiva (`RwLock`, `Mutex`, `channel`), časovače.

**Proč:** De-facto standard async runtime v Rustu. `async fn` v Rustu je jen syntaxe — bez runtimeu by coroutiny neměly kdo je spouštět. Tokio implementuje work-stealing scheduler: pokud je jedno vlákno přetížené, ukradne tasky z jiných vláken.

**Alternativy:** async-std (jednodušší API, menší ekosystém), smol (minimalistický), monoio (io_uring na Linuxu, extrémní výkon).

---

### Serde
**Co je:** Framework pro serializaci a deserializaci. Definuje traity `Serialize` a `Deserialize` — konkrétní formát (JSON, YAML, TOML, Bincode...) je záměnný.

**Proč:** Universální a nulový runtime overhead. `#[derive(Serialize, Deserialize)]` generuje optimální kód při kompilaci — žádná reflexe za běhu jako v Java nebo Python. Výsledný strojový kód je ekvivalentní ručně psanému parseru.

**Alternativy:** Přímo `serde_json::from_str()` bez derive (nutno psát ručně), `simd-json` (SIMD parsing, 2× rychlejší).

---

### Reqwest
**Co je:** HTTP klient pro Rust. Async, s connection poolem, automatická (de)serializace JSON.

**Proč:** Nejpoužívanější HTTP klient v Rust ekosystému. Feature `json` přidá metody `.json::<T>()` pro přímou deserializaci odpovědi do Rust struktury.

**Alternativy:** `hyper` (nízkoúrovňové, na čem reqwest staví), `ureq` (synchronní, bez async overhead pro jednoduché případy).

---

### Cargo
**Co je:** Build tool, správce závislostí a test runner pro Rust. `Cargo.toml` definuje závislosti, `Cargo.lock` zamkne přesné verze.

**Proč:** Součástí Rustu — žádná instalace, žádná konfigurace. `cargo build --release` stáhne závislosti, zkompiluje s optimalizacemi, výsledek je statický binár bez runtime závislostí.

**Cargo.lock vs bez:** `Cargo.lock` commitujeme — zajistí reprodukovatelné buildy. Bez něj by `cargo build` mohl stáhnout jinou (možná breaking) verzi závislosti.

---

## Go — gateway-go

### stdlib only (`net/http`, `net/http/httputil`, `sync`, `encoding/json`)
**Co je:** Go standardní knihovna — součást každé Go instalace, žádné externí závislosti.

**Proč:** Pro gateway-go není frameworku potřeba. `net/http` obsahuje production-ready HTTP server (používá ho i Google v interní infrastruktuře). `httputil.ReverseProxy` implementuje kompletní reverse proxy včetně WebSocket support a streaming.

**`net/http` server je non-trivially dobrý:**
- Každý request → nová goroutina (automaticky)
- Connection keepalive
- Graceful shutdown přes `context`
- HTTP/2 out of the box

**Alternativy:** Gin (nejpopulárnější framework, ~40K stars), Echo, Chi (minimalistický router), Fiber (inspirovaný Express.js).

---

### Go modules (`go.mod`)
**Co je:** Správa závislostí zabudovaná v Go od verze 1.11. `go.mod` definuje modul a závislosti, `go.sum` obsahuje kryptografické hashe.

**Proč:** Standardní součást Go — žádná volba. `go.sum` zabraňuje supply chain útokům: pokud se obsah závislosti změní (i u stejné verze), build selže.

**Zajímavost:** gateway-go má prázdný `go.mod` — žádné `require`. Veškerá funkcionalita pochází ze stdlib.

---

## Python — bff-python

### FastAPI
**Co je:** Moderní async webový framework pro Python. Automaticky generuje OpenAPI dokumentaci, validuje typy přes Pydantic, podporuje dependency injection.

**Proč:** Nejrychlejší Python webový framework (srovnatelný s Node.js). Type hints v signaturách funkcí nejsou jen dokumentace — FastAPI je čte za běhu a automaticky:
- Parsuje query parametry (`q: str = ""`)
- Validuje request body
- Generuje `/docs` endpoint s interaktivní dokumentací

**Alternativy:** Flask (synchronní, jednodušší, obrovský ekosystém), Django (batteries-included, pro větší aplikace), Starlette (na čem FastAPI staví, nižší úroveň).

---

### httpx
**Co je:** Async HTTP klient pro Python. API kompatibilní s `requests`, ale podporuje `async/await`.

**Proč:** `requests` je synchronní — blokuje vlákno při čekání na odpověď. V async FastAPI handleru by to zablokoval celý event loop. `httpx.AsyncClient` je plně async a drží connection pool.

```python
# requests — blokuje event loop ❌
response = requests.get(url)

# httpx — coroutina, uvolní event loop ✓
response = await _client.get(url)
```

**Alternativy:** `aiohttp` (starší, méně přívětivé API), `httpcore` (nízkoúrovňové, na čem httpx staví).

---

### uvicorn
**Co je:** ASGI server — spouštěč pro async Python webové aplikace. FastAPI je ASGI aplikace, uvicorn je ten kdo naslouchá na TCP portu a předává requesty.

**Proč:** Nejrychlejší Python ASGI server. Používá `uvloop` (Cython binding pro libuv — stejná event loop jako Node.js) místo stdlib asyncio event loop → 2–4× vyšší throughput.

**Vztah:** `uvicorn` (server) → `FastAPI` (framework) → tvůj kód (handler)

**Alternativy:** Gunicorn + uvicorn workers (pro produkci s více procesy), Hypercorn (HTTP/2 + HTTP/3 support), Daphne (Django Channels).

---

### uv
**Co je:** Extrémně rychlý Python package manager a virtual environment manager napsaný v Rustu (Astral). Náhrada za `pip` + `venv` + `pip-compile`.

**Proč:** `pip install` trvá sekundy až minuty. `uv` trvá milisekundy — paralelně stahuje, cachuje agresivně, řeší závislosti rychleji. `uv run pytest` automaticky aktivuje venv bez `source .venv/bin/activate`.

**Alternativy:** `pip` + `venv` (standard, pomalé), `poetry` (pěkné, pomalé), `pipenv` (zastaralé).

---

## Infrastruktura

### Docker + Docker Compose
**Co je:** Docker kontejnerizuje aplikace — zabalí kód + závislosti + runtime do izolovaného obrazu. Docker Compose orchestruje více kontejnerů na jednom hostu.

**Multi-stage build (contacts-cpp, search-rust):**
```dockerfile
FROM ubuntu AS builder   # velký image se všemi build nástroji
RUN apt install g++ cmake libpqxx-dev ...
COPY . .
RUN cmake --build .

FROM ubuntu AS runtime   # malý image jen s runtime
COPY --from=builder /app/contacts .  # zkopírujeme jen binár
```
Výsledek: builder image ~1 GB, runtime image ~50 MB.

**`depends_on` + health checks:** Docker Compose počká až service X je `healthy` před spuštěním service Y. Bez toho by contacts-cpp startoval dříve než PostgreSQL a okamžitě selhal.

**Alternativy:** Podman (bez daemona, rootless), Kubernetes (pro produkci s více nody), Nomad.

---

### PostgreSQL 16
**Co je:** Nejpokročilejší open-source relační databáze. ACID transakce, MVCC (Multi-Version Concurrency Control), rozšiřitelnost (PostGIS, pg_vector...).

**Proč:** Standard pro transakční workloady. `libpqxx` má přímou podporu. `postgres:16-alpine` image má jen ~80 MB (Alpine Linux místo Debian).

**MVCC:** PostgreSQL neblokuje čtení zápisem — čtenáři vidí snapshot databáze z doby začátku jejich transakce, pisatelé pracují s novými verzemi řádků. Výsledek: vysoká souběžnost bez lock contention mezi čtenáři a pisateli.

**Volume `postgres_data`:** Data přežijí restart kontejneru. Smazána jsou jen pomocí `docker compose down -v`.

---

### GitHub Actions + MkDocs Material
**Co je:** GitHub Actions je CI/CD platforma. MkDocs je statický generátor dokumentačních webů z Markdown souborů. Material theme přidává moderní design, dark mode, vyhledávání, code copy.

**Workflow:** Push do `main` → Actions spustí `mkdocs build` → výsledné HTML soubory pushne do větve `gh-pages` → GitHub Pages je zveřejní na `navidofek-cmyk.github.io/polycontacts`.

**Proč MkDocs:** Nulová konfigurace pro základní použití. Markdown → profesionální web za 5 minut. Material theme je de-facto standard pro technickou dokumentaci (používá ho FastAPI, Pydantic, Axum...).

**Alternativy:** Docusaurus (React, populární, složitější), Sphinx (Python ekosystém, reStructuredText), GitBook (SaaS).

---

## Přehledová tabulka

| Komponenta | Technologie | Klíčová vlastnost |
|---|---|---|
| HTTP server (C++) | cpp-httplib | Header-only, thread pool |
| JSON (C++) | nlohmann/json | Header-only, intuitivní API |
| PostgreSQL klient | libpqxx | RAII transakce, C++ wrapper |
| Build (C++) | CMake + FetchContent | Správa závislostí bez package manageru |
| HTTP server (Rust) | Axum | Typově bezpečný, Tokio nativní |
| Async runtime | Tokio | Work-stealing scheduler |
| Serializace | Serde | Zero-cost, compile-time |
| HTTP klient (Rust) | Reqwest | Connection pool, JSON feature |
| Build (Rust) | Cargo | Integrovaný, reprodukovatelný |
| HTTP server (Go) | net/http stdlib | Production-ready, goroutiny |
| Reverse proxy | net/http/httputil | Streaming, WebSocket |
| HTTP framework | FastAPI | Async, type hints → OpenAPI |
| HTTP klient (Python) | httpx | Async, connection pool |
| ASGI server | uvicorn | uvloop, nejrychlejší Python server |
| Package manager | uv | Rust-powered, milisekundy |
| Databáze | PostgreSQL 16 | MVCC, ACID, Alpine image |
| Kontejnery | Docker Compose | Multi-stage build, health checks |
| CI/CD + docs | GitHub Actions + MkDocs | Push → web automaticky |

---

## OOP — objektově orientované programování v projektu

Každý jazyk implementuje OOP jinak. Projekt ukazuje čtyři různé přístupy ke stejnému problému (enkapsulace stavu + metody nad ním).

### C++ — třídy s explicitní kontrolou přístupu

```cpp
class ConnPool {
    std::queue<pqxx::connection*> pool_;  // private — skryto před okolím
    std::mutex mtx_;
    std::condition_variable cv_;
    int size_;

public:
    explicit ConnPool(const std::string& dsn, int size);  // konstruktor
    pqxx::connection* acquire();   // veřejné rozhraní
    void release(pqxx::connection* conn);
    ~ConnPool();                   // destruktor — RAII cleanup
};
```

**`private` / `public`:** Explicitní kontrola přístupu. `pool_` je implementační detail — okolní kód nemůže přistoupit přímo, musí použít `acquire()` / `release()`. Invariant (pool je vždy konzistentní) je zaručen třídou, ne klientem.

**`explicit` konstruktor:** Zabraňuje implicitním konverzím — `ConnPool pool = "postgresql://..."` selže na kompilaci. Musí být `ConnPool pool("postgresql://...")`.

**Destruktor `~ConnPool()`:** Volá se automaticky při zániku objektu (RAII). Uzavře všechna DB spojení — žádný resource leak i při výjimce.

### Rust — struct + impl (bez dědičnosti)

```rust
pub struct SearchIndex {
    index: HashMap<String, Vec<String>>,
    contacts: Vec<Contact>,
}

impl SearchIndex {
    pub fn new() -> Self { ... }        // "konstruktor"
    pub fn rebuild(&mut self, ...) { }  // mutuje self
    pub fn search(&self, ...) -> Vec<Contact> { }  // čte self
}
```

Rust **nemá dědičnost**. Místo ní:
- **Struct** = data (jako `class` bez metod)
- **`impl`** = metody na struct
- **Trait** = sdílené chování (jako interface)

`&self` vs `&mut self` v signatuře metody vyjadřuje záměr — kompilátor ověří, že `search()` skutečně data nemění. Nelze zapomenout na thread safety — `&mut self` nelze předat do více vláken najednou.

### Go — struct + metody (žádné třídy)

```go
type Gateway struct {
    mu       sync.RWMutex
    services map[string]*ServiceEntry
}

func (g *Gateway) Register(entry ServiceEntry) error { ... }
func (g *Gateway) GetAll() []ServiceEntry { ... }
```

Go nemá třídy — jen struct + metody s přijímačem (`g *Gateway`). Enkapsulace přes balíčky: malé písmeno = privátní (`mu`, `services`), velké = veřejné (`Register`, `GetAll`).

**Pointer receiver `*Gateway`:** Metody mění stav — musí pracovat s originálním objektem, ne kopií. Value receiver by zkopíroval celý Gateway včetně mutexu — nekorektní.

### Python — dynamické třídy

```python
class SearchIndex:
    def __init__(self):
        self._index = {}      # konvence: _ = "privátní"
        self._contacts = []

    def rebuild(self, contacts):
        self._index = self._build(contacts)

    def search(self, query):
        return self._lookup(query)
```

Python nemá skutečnou enkapsulaci — `_index` je konvence ("prosím nešahej"), ne vynucení kompilátorem. Kdokoliv může přistoupit k `obj._index` přímo.

**Dynamické typy:** Python nevyžaduje deklaraci typů — `self._index = {}` může být v dalším řádku přepsán čímkoliv. Typové anotace (`: dict[str, list]`) jsou jen dokumentace, ne kontrola.

---

## SQL — schéma a dotazy v projektu

### Schéma databáze

```sql
CREATE TABLE contacts (
    id         TEXT PRIMARY KEY,           -- UUID v4, generuje contacts-cpp
    first_name TEXT NOT NULL,
    last_name  TEXT NOT NULL,
    email      TEXT NOT NULL DEFAULT '',
    category   TEXT NOT NULL DEFAULT 'Other'
);

CREATE TABLE phone_numbers (
    id         BIGSERIAL PRIMARY KEY,       -- auto-increment integer
    contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,
    label      TEXT NOT NULL DEFAULT '',
    number     TEXT NOT NULL
);
```

**`TEXT PRIMARY KEY` vs `SERIAL`:** UUID jako primární klíč (TEXT) je horší pro výkon B-tree indexu než celočíselný klíč (sekvenční INT je vždy na konci stromu). Výhoda: UUID lze generovat bez databáze — contacts-cpp generuje id lokálně a vloží ho. Žádný round-trip pro získání klíče.

**`REFERENCES contacts(id) ON DELETE CASCADE`:** Cizí klíč s kaskádovým mazáním. Smazání kontaktu automaticky smaže všechna jeho telefonní čísla — žádný orphan records. PostgreSQL to vynucuje transaktivně.

**`BIGSERIAL`:** Zkratka pro `BIGINT GENERATED ALWAYS AS IDENTITY` — automaticky inkrementovaný 64-bit integer. Sekvenční klíč pro phone_numbers je efektivnější než UUID (menší index, cache-friendly).

### Parametrizované dotazy (SQL injection ochrana)

```cpp
// Správně — parametrizovaný dotaz
txn.exec_params(
    "INSERT INTO contacts (id, first_name, last_name) VALUES ($1, $2, $3)",
    contact.id, contact.first_name, contact.last_name
);

// Špatně — string concatenation → SQL injection
txn.exec("INSERT INTO contacts VALUES ('" + id + "', '" + name + "')");
```

**SQL injection:** Útočník pošle jako `first_name` hodnotu `'; DROP TABLE contacts; --`. String concatenation vloží tento kód přímo do SQL příkazu. `exec_params` posílá data a SQL odděleně — databáze je nezaměňuje, data jsou vždy data.

**`$1`, `$2`...** jsou PostgreSQL placeholdery. libpqxx je nahradí hodnotami přes protokol, ne textovou substitucí.

### JOIN a N+1 problém

```sql
-- Načti kontakty s telefony — jeden dotaz (správně)
SELECT c.*, p.label, p.number
FROM contacts c
LEFT JOIN phone_numbers p ON p.contact_id = c.id
ORDER BY c.last_name, c.first_name;

-- Alternativa: N+1 problém (špatně)
SELECT * FROM contacts;               -- 1 dotaz
-- pro každý kontakt zvlášť:
SELECT * FROM phone_numbers WHERE contact_id = $1;  -- N dotazů
```

**N+1 problém:** 30 kontaktů = 31 SQL dotazů místo 1. Při 1000 kontaktech = 1001 dotazů. Každý dotaz = network round-trip na databázi (~1ms) → sekvenčně 1 sekunda jen na overhead. JOIN řeší vše jedním dotazem.

contacts-cpp používá JOIN při načítání, ale pro jednoduchost implementace ukládá telefony zvlášť přes separátní `INSERT` po vložení kontaktu — přijatelný kompromis pro malý dataset.

### Transakce a ACID

```cpp
pqxx::work txn(conn);  // BEGIN

// 1. Vlož kontakt
txn.exec_params("INSERT INTO contacts ...", ...);

// 2. Vlož telefony
for (auto& phone : contact.phones) {
    txn.exec_params("INSERT INTO phone_numbers ...", ...);
}

txn.commit();  // COMMIT — buď vše, nebo nic
```

**ACID:**
- **Atomicity:** `commit()` potvrdí vše najednou. Při chybě po prvním INSERT ale před druhým — ROLLBACK zruší i první INSERT. Databáze zůstane konzistentní.
- **Consistency:** Cizí klíč `REFERENCES contacts` nelze porušit — INSERT do phone_numbers bez existujícího contact_id selže.
- **Isolation:** Souběžné transakce se navzájem neovlivní (PostgreSQL MVCC).
- **Durability:** Po `commit()` jsou data na disku i při pádu serveru (WAL — Write-Ahead Log).
