# bff-python — Backend for Frontend

## BFF pattern

Backend for Frontend je vrstva, která sedí mezi prohlížečem a backendovými službami. Prohlížeč volá jen jednu adresu (`http://localhost:8989`) a neví nic o interní síti Docker kontejnerů. BFF pak požadavky přeposílá na správný backend nebo je agreguje z více zdrojů najednou.

Bez BFF by prohlížeč musel:

- znát adresy všech tří backendů (contacts-cpp, search-rust, gateway-go)
- řešit CORS hlavičky pro každý z nich
- sám paralelizovat volání pro stránku se souhrnnými statistikami

S BFF to vše řeší server — prohlížeč dostane jednu konzistentní API.

## asyncio.gather pro /api/stats

Endpoint `/api/stats` musí získat statistiky ze tří různých backendů. Při sekvenčním volání by celková latence byla součet tří timeoutů (až 15 s). S `asyncio.gather` probíhají všechna tři volání paralelně a čeká se jen na to nejpomalejší.

```python
async def _fetch_stats(name: str, base_url: str) -> dict:
    try:
        r = await _client.get(f"{base_url}/stats", timeout=5.0)
        data = r.json() if r.status_code == 200 else {}
        return {"service": name, **data}
    except Exception as exc:
        # Jeden nedostupný backend nezabrání zobrazení ostatních
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

Každý backend dostane timeout 5 s. Celková latence `/api/stats` odpovídá nejpomalejší ze tří odpovědí, ne jejich součtu.

## Proxy helper

Většina endpointů BFF jen přeposílá požadavek beze změny. Sdílená funkce `_proxy` to řeší jednotně:

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
            # "host" by backend odmítl — neshoduje se s jeho doménou
            # "content-length" přepočítá httpx automaticky z těla
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

Sdílený `httpx.AsyncClient` (s connection poolingem) se vytváří jednou při startu aplikace — opakované použití je výrazně rychlejší než zakládání nového klienta per požadavek.

## API

| Metoda | Cesta BFF | Cíl | Popis |
|---|---|---|---|
| `GET` | `/api/contacts?q=` | contacts-cpp `/contacts` | Seznam kontaktů |
| `POST` | `/api/contacts` | contacts-cpp `/contacts` | Vytvoř kontakt |
| `PUT` | `/api/contacts/{id}` | contacts-cpp `/contacts/{id}` | Aktualizuj kontakt |
| `DELETE` | `/api/contacts/{id}` | contacts-cpp `/contacts/{id}` | Smaž kontakt |
| `GET` | `/api/search?q=` | search-rust `/search` | Fulltextové hledání |
| `GET` | `/api/dedup` | contacts-cpp `/dedup` | Potenciální duplicity |
| `GET` | `/api/analytics` | contacts-cpp `/analytics` | Statistiky adresáře |
| `GET` | `/api/export/vcard` | contacts-cpp `/export/vcard` | Export vCard |
| `POST` | `/api/import/vcard` | contacts-cpp `/import/vcard` | Import vCard |
| `GET` | `/api/services` | gateway-go `/services` | Registry služeb |
| `GET` | `/api/topology` | gateway-go `/topology` | Graf závislostí |
| `GET` | `/api/stats` | všechny 3 × `/stats` | Agregované statistiky |
| `GET` | `/health` | — | Liveness probe |
| `GET` | `/` | statický soubor | Hlavní HTML stránka |
