# gateway-go — Service Registry

## Zodpovědnost

Gateway-go udržuje živý seznam registrovaných backendových služeb, periodicky ověřuje jejich zdraví a exponuje ho přes REST API. Navíc funguje jako reverse proxy — umožňuje volat libovolný backend přes gateway URL — a sleduje topologii meziservisních závislostí pro vizualizaci v UI.

## Proč stdlib only

Gateway-go nepoužívá žádné externí závislosti — jen `net/http`, `net/http/httputil`, `sync` a `sync/atomic` ze standardní knihovny Go.

Důvod je pragmatický: každá přidaná závislost může přinést **diamond dependency problem** — dva balíčky závisejí na různých verzích téhož modulu a `go.sum` to musí nějak vyřešit. Pro komponentu, která dělá jen HTTP routing a JSON serializaci, je přínos frameworku mizivý a cena (složitost `go.mod`, bezpečnostní audit transitivních závislostí) je zbytečná.

Výsledek: gateway má nulové `go.mod` závislosti, zkompiluje se do jednoho statického bináru, buildovací cache nikdy nezastaralá.

## Reverse proxy

Gateway stripuje prefix `/{service-name}` z URL a přepošle zbytek na backend. Klient tak nemusí znát interní adresu backendu.

```
Klient volá:   GET /contacts-cpp/contacts?q=jana
Gateway předá: GET /contacts?q=jana  →  http://contacts-cpp:8080
```

Implementace používá `strings.SplitN` s limitem 2 — zbytek cesty za prvním lomítkem zůstane celý pohromadě:

```go
func (g *Gateway) handleProxy(w http.ResponseWriter, r *http.Request) {
    parts := strings.SplitN(strings.TrimPrefix(r.URL.Path, "/"), "/", 2)
    serviceName := parts[0]  // "contacts-cpp"

    // ... najdeme proxy pro tuto službu ...

    // Stripujeme prefix, backend dostane čistou cestu
    if len(parts) > 1 {
        r.URL.Path = "/" + parts[1]  // "/contacts?q=jana"
    } else {
        r.URL.Path = "/"
    }
    proxy.ServeHTTP(rw, r)
}
```

`RawPath` (percent-encoded verze) se ošetřuje stejně, aby se zabránilo dvojitému dekódování `%2F` → `%252F`.

## Health checker

Gateway spustí jednu background goroutinu s `time.NewTicker(10s)`. Při každém tiknutí pořídí snapshot názvů registrovaných služeb pod RLock a pak pro každou službu spustí **samostatnou goroutinu**.

Proč goroutina per service? Pokud by se health checky prováděly sekvenčně a jedna služba by byla pomalá nebo nedostupná, timeout 5 s by se násobil počtem služeb. S goroutinami běží všechny kontroly paralelně a celý cyklus trvá max. 5 s bez ohledu na počet služeb.

Za healthy se považuje jakákoliv odpověď s HTTP statusem < 500. 4xx (např. 404 na `/health`) je stále zdravá služba — mluví s námi, jen možná nemá ten endpoint.

## API

| Metoda | Cesta | Popis |
|---|---|---|
| `POST` | `/services` | Zaregistruj službu `{name, url, health_path}` |
| `GET` | `/services` | Seznam všech služeb se zdravotním stavem a čítači |
| `DELETE` | `/services/{name}` | Odregistruj službu |
| `GET` | `/topology` | Graf závislostí `{"edges":[{"from":"A","to":"B"}]}` |
| `POST` | `/topology/edge` | Přidej hranu `{from, to}` |
| `GET` | `/health` | `{"status":"ok","service":"gateway-go","uptime_s":N}` |
| `GET` | `/stats` | Počty požadavků a chyb per service |
| `/*` | `/{service}/...` | Reverse proxy na registrovaný backend |
