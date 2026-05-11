# Architektura

## Proč microservices?

Monolitická aplikace je jednodušší na vývoj, ale obtížnější na škálování a údržbu. Microservices rozdělují systém na **nezávisle nasaditelné jednotky** — každá má vlastní zodpovědnost, vlastní proces a vlastní technologický stack.

Tři principy za tímto projektem:

**Separation of concerns.** `contacts-cpp` řeší persistence, `search-rust` řeší vyhledávání, `gateway-go` řeší discovery. Žádná služba nezná interní implementaci jiné — komunikují pouze přes HTTP/JSON kontrakty.

**Polyglot persistence a jazyk.** Každý problém si vybere nejvhodnější nástroj. Vyhledávání potřebuje rychlý in-memory index → Rust. CRUD s transakcemi → C++ s libpqxx. Orchestrace a proxy → Go stdlib. UI a lepidlo → Python/FastAPI.

**Nezávislé škálování.** Kdybychom search-rust dostali pod větší zátěž, nasadíme více instancí bez dotyku ostatních služeb. V tomto projektu to neřešíme (jeden Docker host), ale architektura to umožňuje.

## BFF pattern (Backend for Frontend)

`bff-python` implementuje pattern **Backend for Frontend** — jeden agregační backend navržený specificky pro jednoho klienta (webový prohlížeč).

Bez BFF by prohlížeč musel volat tři různé backendy (contacts-cpp, search-rust, gateway-go), řešit CORS, znát jejich adresy a agregovat výsledky sám. BFF tuto komplexitu skryje za jednu adresu `:8989`.

Klíčová vlastnost: `/api/stats` volá tři backendy **paralelně** přes `asyncio.gather`. Výsledná latence je `max(t1, t2, t3)`, ne `t1 + t2 + t3`. Bez asyncio by stejný endpoint trval 3× déle.

## Service Registry pattern

`gateway-go` implementuje **Service Registry** — centrální katalog kde běžící služby. Každá služba se při startu zaregistruje (`POST /services`) a gateway ji od té chvíle pravidelně pinguje (`GET /health` každých 10 s) a měří latenci.

Alternativou by bylo statické nastavení adres v config souboru. Registry přidává dynamiku — pokud kontejner padne a nastartuje na jiné adrese, stačí se znovu zaregistrovat. V tomto projektu jsou adresy statické (Docker Compose DNS), ale pattern zůstává stejný jako v produkčních systémech (Consul, etcd).

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

## CAP theorem v tomto systému

CAP theorem říká, že distribuovaný systém může garantovat současně nejvýše dvě ze tří vlastností: **Consistency** (všichni vidí stejná data), **Availability** (systém vždy odpovídá) a **Partition tolerance** (systém funguje i při výpadku sítě mezi uzly).

Polycontacts volí **AP** — dostupnost nad konzistencí:

- `contacts-cpp` zapíše kontakt do PostgreSQL a **asynchronně** notifikuje `search-rust`
- V okně mezi zápisem a doručením notifikace vrátí search stará data
- Systém preferuje odpovědět rychle a možná trochu zastarale před čekáním na plnou synchronizaci

Tato forma nekonzistence se nazývá **eventual consistency** — systém se časem dostane do konzistentního stavu, ale nezaručuje to okamžitě. Pro adresář kontaktů je to přijatelný kompromis.

## Fire-and-forget pattern

`contacts-cpp` po každém POST/PUT spustí **detached thread** který pošle `POST /index` na `search-rust`. Thread není nijak sledován — pokud selže (search-rust je dole, síť je přetížená), chyba se zahodí.

```
POST /contacts
    │
    ├── uloží do PostgreSQL  (synchronně, musí uspět)
    └── spustí detached thread → POST /index na search-rust  (asynchronně, best-effort)
         │
         └── chyba? zahodí se. Žádný retry, žádný log.
```

Výhoda: zápis kontaktu je rychlý, neblokuje ho dostupnost search-rust.
Nevýhoda: search index může zaostávat za databází.

V produkčních systémech se tento pattern řeší **message queue** (Kafka, RabbitMQ) — zpráva se uloží durably a search-rust ji zpracuje až bude ready. My záměrně používáme jednodušší přístup.

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
