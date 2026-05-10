# bff-python — Průvodce kódem

## Přehled a BFF pattern

**Backend-for-Frontend (BFF)** je architektonický vzor: server, který sedí mezi prohlížečem a interní sítí backendových služeb. Prohlížeč nezná nic o kontejnerech, interních adresách ani o tom, kolik backendů existuje — komunikuje jen s jednou adresou (`http://localhost:8989`).

Bez BFF by prohlížeč musel:

- znát adresy všech tří backendů (`contacts-cpp:8080`, `search-rust:8081`, `gateway-go:9000`)
- řešit CORS hlavičky pro každý z nich zvlášť
- sám paralelizovat volání při sestavování souhrnné stránky (např. `/api/stats`)

BFF tyto problémy řeší na straně serveru. Prohlížeč dostane jednu konzistentní API bez závislosti na topologii interní sítě.

`bff-python` je napsán ve FastAPI a komunikuje s backendy přes sdíleného `httpx.AsyncClient`. Zároveň obsluhuje statické soubory frontendu — prohlížeč stáhne HTML/JS/CSS ze stejné adresy jako volá API.

---

## Konfigurace a env proměnné

```python
GATEWAY_URL  = os.getenv("GATEWAY_URL",  "http://localhost:9000")
CONTACTS_URL = os.getenv("CONTACTS_URL", "http://localhost:8080")
SEARCH_URL   = os.getenv("SEARCH_URL",   "http://localhost:8081")
PORT         = int(os.getenv("PORT",     "8989"))

STATIC_DIR = Path(__file__).parent / "static"
```

Každá proměnná používá `os.getenv(název, výchozí_hodnota)` — dvouhodnotová varianta, která vrátí výchozí hodnotu pokud proměnná prostředí není nastavena. Tento vzor zajistí:

- **Lokální vývoj** — `python main.py` funguje bez nastavení čehokoliv, výchozí adresy míří na `localhost`.
- **Docker / Kubernetes** — orchestrátor nastaví proměnné prostředí (`CONTACTS_URL=http://contacts-cpp:8080`) a aplikace je automaticky použije.

`STATIC_DIR` je konstruován relativně vůči souboru `main.py` pomocí `Path(__file__).parent` — funguje správně bez ohledu na to, odkud je aplikace spuštěna (cwd se nemění).

---

## lifespan context manager

```python
@asynccontextmanager
async def lifespan(app: FastAPI):
    global _client
    _client = httpx.AsyncClient(timeout=10.0)

    try:
        await _client.post(
            f"{GATEWAY_URL}/services",
            json={
                "name": "bff-python",
                "url": "http://bff-python:8989",
                "health_path": "/health",
            },
        )
    except Exception:
        pass

    yield

    await _client.aclose()


app = FastAPI(title="bff-python", lifespan=lifespan)
```

### `@asynccontextmanager` — starý i nový způsob

FastAPI původně používal `on_startup` / `on_shutdown` event handlery. Od verze 0.95+ je preferovaný způsob `lifespan` — jeden kontextový manažer, který zahrnuje obě fáze životního cyklu. `@asynccontextmanager` z `contextlib` umožňuje zapsat startup a shutdown kód do jedné funkce s `yield` jako dělítkem.

### Proč sdílený `httpx.AsyncClient`

```python
_client = httpx.AsyncClient(timeout=10.0)  # jednou při startu
```

`httpx.AsyncClient` interně spravuje **connection pool** — udržuje otevřená TCP spojení na každý backend a znovu je používá pro další požadavky. Vytvoření nového klienta per požadavek by znamenalo:

1. Nové TCP spojení (3-way handshake) pro každý požadavek — latence desítek ms navíc.
2. TLS handshake pokud backend používá HTTPS.
3. Zbytečnou alokaci a garbage collection.

Sdílený klient tyto náklady amortizuje přes všechny příchozí požadavky.

### Registrace u gateway — proč `try/except pass`

```python
try:
    await _client.post(f"{GATEWAY_URL}/services", json={...})
except Exception:
    pass  # gateway may not be up yet; ignore
```

Pořadí startu Docker kontejnerů není deterministické. BFF se může spustit dříve než gateway, síť nemusí být ještě připravená, nebo gateway může být v restart loopě. Selhání registrace není fatální — gateway má vlastní health checker, který BFF časem objeví. Ignorování výjimky (`pass`) je zde správné rozhodnutí: chceme, aby BFF nastartoval i bez gateway.

!!! warning "Proč ne `except Exception as e: log.warning(...)`"
    V produkčním systému bychom výjimku logovali pro snadnější debugging. Zde je `pass` přijatelný zkratkovitý zápis pro vývojové prostředí.

### `yield` — startup vs shutdown

```python
async def lifespan(app: FastAPI):
    # === STARTUP ===
    _client = httpx.AsyncClient(...)
    try:
        await _client.post(...)  # registrace u gateway
    except Exception:
        pass

    yield  # <-- zde FastAPI spustí server a začne přijímat požadavky

    # === SHUTDOWN ===
    await _client.aclose()  # korektní uzavření spojení
```

Kód před `yield` se spustí jednou při startu — inicializace sdílených zdrojů. Kód po `yield` se spustí jednou při vypnutí — úklid. `_client.aclose()` korektně uzavře všechna otevřená TCP spojení a uvolní porty, místo aby čekal na garbage collector.

---

## `_proxy()` helper

```python
async def _proxy(request: Request, url: str, **kwargs) -> Response:
    body = await request.body()
    upstream = await _client.request(
        method=request.method,
        url=url,
        content=body,
        headers={
            k: v
            for k, v in request.headers.items()
            if k.lower() not in ("host", "content-length")
        },
        **kwargs,
    )
    return Response(
        content=upstream.content,
        status_code=upstream.status_code,
        media_type=upstream.headers.get("content-type", "application/json"),
    )
```

Řádek po řádku:

**`body = await request.body()`**
FastAPI tělo požadavku nenačítá automaticky — `request.body()` je coroutine, která ho přečte z síťového streamu. Explicitní `await` je nutné: bez něj bychom předali prázdné tělo. Pro `GET` požadavky vrátí prázdný `bytes`, což je správné.

**`method=request.method`**
Přeposílá HTTP metodu beze změny — `GET`, `POST`, `PUT`, `DELETE` vše putuje k upstream službě.

**`content=body`**
`httpx` rozlišuje `content=` (raw bytes) a `json=` (automatická serializace). Používáme `content=` protože tělo jsme přečetli jako raw bytes a nechceme ho znovu parsovat ani enkódovat.

**Filtrování headers:**
```python
if k.lower() not in ("host", "content-length")
```

- **`host`** — HTTP header `Host` říká serveru, na jakou doménu požadavek míří. BFF pošle `Host: localhost:8989` (svou vlastní adresu). Upstream backend (`contacts-cpp:8080`) by tento header odmítl nebo špatně interpretoval, protože se neshoduje s jeho konfigurací. `httpx` automaticky nastaví správný `Host` pro cílovou URL.
- **`content-length`** — délka těla v bytech. `httpx` ji přepočítá automaticky z předaného `content=body`. Pokud bychom přeposlali původní `content-length` a tělo bylo přeformátováno, došlo by k neshodě délky a upstream by vrátil `400 Bad Request`.

**`**kwargs`**
Volitelné klíčové argumenty předané přímo do `httpx.AsyncClient.request()`. Endpointy je používají pro `params=`:

```python
return await _proxy(request, f"{CONTACTS_URL}/dedup", params={"threshold": threshold})
```

`params={"threshold": 0.85}` se přemění na query string `?threshold=0.85` v cílové URL — upstream vidí parametr přesně tak, jak ho klient poslal na BFF.

**`Response(...)`**
Vrátíme FastAPI `Response` objekt s raw `content` z upstream odpovědi. Nepoužíváme `JSONResponse` záměrně — neznáme a nechceme modifikovat obsah odpovědi, jen ho přeposlat. `media_type` bereme z upstream `Content-Type` headeru.

---

## Endpointy — projdi každý

### `/health`

```python
@app.get("/health")
async def health():
    return {"status": "ok", "service": "bff-python"}
```

Jednoduchý liveness endpoint. Gateway ho volá každých 10 sekund, aby věděla, zda je tato instance nahoře. Neověřuje dostupnost backendů — jen potvrzuje, že BFF sám běží.

### `/` (index)

```python
@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")
```

Vrátí hlavní HTML soubor SPA (Single-Page Application). Veškerá interakce s API probíhá přes JavaScript uvnitř stránky. Explicitní route musí existovat, protože `StaticFiles` mount (mountovaný jako poslední) by pro `/` vrátil directory listing nebo `403`, ne `index.html`.

### `/api/contacts` — CRUD

```python
@app.get("/api/contacts")
async def get_contacts(request: Request, q: str = ""):
    return await _proxy(request, f"{CONTACTS_URL}/contacts", params={"q": q})

@app.post("/api/contacts")
async def create_contact(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/contacts")

@app.put("/api/contacts/{contact_id:path}")
async def update_contact(request: Request, contact_id: str):
    return await _proxy(request, f"{CONTACTS_URL}/contacts/{contact_id}")

@app.delete("/api/contacts/{contact_id:path}")
async def delete_contact(request: Request, contact_id: str):
    return await _proxy(request, f"{CONTACTS_URL}/contacts/{contact_id}")
```

Přímé přesměrování na `contacts-cpp`. Parametr `:path` v `{contact_id:path}` umožňuje ID obsahovat lomítka — bez něj by FastAPI vrátil `404` pro ID jako `user/123`.

### `/api/search`

```python
@app.get("/api/search")
async def search(request: Request, q: str = ""):
    return await _proxy(request, f"{SEARCH_URL}/search", params={"q": q})
```

Přesměruje fulltext dotaz do `search-rust`, který udržuje invertovaný index nad kontakty.

### `/api/db`

```python
@app.get("/api/db")
async def db_tables(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/db/tables")
```

Diagnostický endpoint — vrátí raw obsah PostgreSQL tabulek pro vývojářský pohled v UI.

### `/api/dedup`

```python
@app.get("/api/dedup")
async def dedup(request: Request, threshold: float = 0.85):
    return await _proxy(request, f"{CONTACTS_URL}/dedup", params={"threshold": threshold})
```

Najde potenciální duplicity; `threshold` (0–1) určuje minimální míru podobnosti. Výchozí hodnota 0.85 je praktický kompromis — nízká hodnota způsobí příliš mnoho false positives, vysoká přehlédne reálné duplicity.

### `/api/analytics`

```python
@app.get("/api/analytics")
async def analytics(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/analytics")
```

Vrátí agregované metriky (rozdělení kategorií, počty kontaktů podle příjmení atd.) z `contacts-cpp`.

### `/api/export/vcard` a `/api/import/vcard`

```python
@app.get("/api/export/vcard")
async def export_vcard(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/export/vcard")

@app.post("/api/import/vcard")
async def import_vcard(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/import/vcard")
```

Export vrátí `.vcf` soubor se všemi kontakty. Import přijme multipart nebo raw vCard tělo a dávkově naimportuje kontakty do `contacts-cpp`.

### `/api/services` a `/api/topology`

```python
@app.get("/api/services")
async def services(request: Request):
    return await _proxy(request, f"{GATEWAY_URL}/services")

@app.get("/api/topology")
async def topology(request: Request):
    return await _proxy(request, f"{GATEWAY_URL}/topology")
```

Přesměrují dotaz přímo na `gateway-go` — registry registrovaných služeb a graf závislostí pro vizualizaci v UI.

### `/api/stats` — paralelní fan-out

Toto je jediný endpoint BFF, který **agreguje data z více zdrojů** namísto prostého přesměrování.

```python
async def _fetch_stats(name: str, base_url: str) -> dict:
    try:
        r = await _client.get(f"{base_url}/stats", timeout=5.0)
        data = r.json() if r.status_code == 200 else {}
        return {"service": name, **data}
    except Exception as exc:
        return {"service": name, "error": str(exc)}


@app.get("/api/stats")
async def stats():
    targets = [
        ("contacts-cpp", CONTACTS_URL),
        ("search-rust",  SEARCH_URL),
        ("gateway-go",   GATEWAY_URL),
    ]
    results = await asyncio.gather(
        *[_fetch_stats(name, url) for name, url in targets]
    )
    return JSONResponse(content=list(results))
```

**`_fetch_stats` — degradovaný záznam místo výjimky:**

```python
except Exception as exc:
    return {"service": name, "error": str(exc)}
```

Pokud `contacts-cpp` neodpovídá, `_fetch_stats` vrátí `{"service": "contacts-cpp", "error": "Connection refused"}` místo vyvolání výjimky. To je **degraded response** vzor — jeden nedostupný backend nezabránil zobrazení statistik ostatních dvou. Caller (`stats()`) dostane výsledky ze všech tří volání bez ohledu na to, zda selhaly.

**`asyncio.gather` — proč celková latence = max(3 backends):**

```python
results = await asyncio.gather(
    _fetch_stats("contacts-cpp", CONTACTS_URL),  # spustí se okamžitě
    _fetch_stats("search-rust",  SEARCH_URL),    # spustí se okamžitě
    _fetch_stats("gateway-go",   GATEWAY_URL),   # spustí se okamžitě
)
# čeká, dokud všechny tři nedokončí — celková latence = max(latence A, B, C)
```

`asyncio.gather` spustí všechny coroutines **souběžně** v rámci jednoho event loop threadu — ne paralelně v OS smyslu, ale přepíná mezi nimi v každém `await` bodě. Každé `_fetch_stats` čeká na síťovou odpověď (`await _client.get(...)`) — v době čekání event loop obslouží ostatní coroutines. Výsledkem je, že všechna tři volání probíhají překrytě:

```
Sekvenčně:    |--A(3s)--|--B(2s)--|--C(5s)--|  = 10s celkem
asyncio.gather: |--A(3s)--|
               |--B(2s)--|
               |------C(5s)------|  = 5s celkem (max)
```

!!! info "asyncio není threading"
    `asyncio.gather` nevytváří vlákna ani procesy. Vše běží v jednom Python threadu. Paralelismus vzniká tím, že I/O operace (síťová volání) uvolní event loop pro ostatní coroutines. CPU-bound operace by se takto nezrychlily.

---

## StaticFiles mount

```python
app.mount("/", StaticFiles(directory=str(STATIC_DIR), html=True), name="static")
```

`StaticFiles` mount musí být **poslední** — FastAPI prochází routes v pořadí registrace a vrátí první shodu. Pokud by `StaticFiles` byl mountován dříve, zachytil by všechny požadavky na `/` (včetně `/api/contacts`, `/health` atd.) a vrátil by statický soubor nebo `404`.

Parametr `html=True` aktivuje fallback chování: pokud soubor `static/api/contacts` neexistuje, `StaticFiles` vrátí `404` a FastAPI pokračuje hledáním další shody — takže explicitní route výše zachytí požadavek správně. Bez `html=True` by `StaticFiles` vrátil vlastní `404` okamžitě.

!!! note "Proč mount na `/` a ne na `/static`"
    Frontend je SPA — HTML, JS a CSS jsou servovány ze stejné základní adresy jako API. Prohlížeč tak nemusí řešit cross-origin požadavky a celá aplikace funguje na jednom portu.

---

## Jak testovat

```bash
cd services/bff-python && uv run pytest tests/ -v
```

Spustí unit testy BFF. `uv run` zajistí správné virtuální prostředí bez nutnosti `venv activate`.

```bash
curl http://localhost:8989/api/analytics
```

Vrátí agregované metriky z `contacts-cpp` — rozdělení kontaktů podle kategorií, počty atd.

```bash
curl http://localhost:8989/api/dedup?threshold=0.8
```

Najde potenciální duplicity s mírou podobnosti ≥ 80 %. Nižší threshold vrátí více výsledků (více false positives), vyšší méně (přísnější shoda).

```bash
curl http://localhost:8989/api/stats
```

Agregované statistiky ze všech tří backendů souběžně — výsledek by měl přijít do ~5 sekund bez ohledu na to, kolik backendů odpovídá.

```bash
curl -X POST http://localhost:8989/api/contacts \
  -H "Content-Type: application/json" \
  -d '{"name": "Jana Nováková", "email": "jana@example.com"}'
```

Vytvoří nový kontakt — BFF předá tělo beze změny do `contacts-cpp`.
