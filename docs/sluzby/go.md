# gateway-go — Průvodce kódem

## Přehled

`gateway-go` je centrální API gateway celého systému. Plní tři role najednou:

1. **Service registry** — udržuje živý seznam backendových služeb s jejich zdravotním stavem.
2. **Reverse proxy** — přeposílá příchozí požadavky na správný backend podle URL prefixu.
3. **Topology tracker** — zaznamenává, kdo volá koho, pro vizualizaci závislostí v UI.

Celý gateway je napsán v čistém Go bez jediné externí závislosti — jen stdlib.

---

## go.mod a proč stdlib only

```
module gateway-go

go 1.22
```

`go.mod` je záměrně prázdný — žádná sekce `require`. Veškerá funkcionalita pochází ze standardní knihovny Go.

Proč? Každá přidaná závislost přináší:

- **Diamond dependency problem** — dva balíčky závisejí na různých verzích stejného modulu a `go.sum` to musí nějak vyřešit.
- **Bezpečnostní audit** — transitivní závislosti jsou zdrojem CVE, které musíš sledovat i když je sám nepoužíváš.
- **Buildovací cache** — při každé změně závislosti se invalidují vrstvy Docker cache.

Pro komponentu, která dělá HTTP routing, JSON serializaci a reverse proxy, je přínos frameworku mizivý. Klíčový balíček `net/http/httputil` je production-ready součástí Go stdlib — Google ho používá ve vlastní infrastruktuře. Gateway se kompiluje do jediného statického bináru, který nepotřebuje žádné runtime prostředí.

---

## ServiceEntry struct

```go
type ServiceEntry struct {
    Name         string    `json:"name"`
    URL          string    `json:"url"`
    HealthPath   string    `json:"health_path"`
    Routes       []string  `json:"routes"`
    Status       string    `json:"status"`
    LatencyMs    int64     `json:"latency_ms"`
    LastChecked  time.Time `json:"last_checked"`
    RegisteredAt time.Time `json:"registered_at"`

    RequestCount atomic.Int64 `json:"-"`
    ErrorCount   atomic.Int64 `json:"-"`
}
```

Každé pole má svůj důvod:

| Pole | Typ | Proč |
|---|---|---|
| `Name` | `string` | Primární klíč — identifikátor v mapě `services` i v URL prefixu |
| `URL` | `string` | Interní adresa backendu v Docker síti, např. `http://contacts-cpp:8080` |
| `HealthPath` | `string` | Cesta health endpointu; výchozí `/health` pokud není uvedena |
| `Routes` | `[]string` | Informativní seznam cest, které backend obsluhuje (pro UI, ne pro routing) |
| `Status` | `string` | `"healthy"` / `"unhealthy"` / `"unknown"` — výsledek posledního health checku |
| `LatencyMs` | `int64` | Latence posledního health checku v milisekundách |
| `LastChecked` | `time.Time` | Kdy byl proveden poslední health check |
| `RegisteredAt` | `time.Time` | Kdy byla služba zaregistrována — pro audit a debugging |
| `RequestCount` | `atomic.Int64` | Počet přeposlaných požadavků — atomický čítač |
| `ErrorCount` | `atomic.Int64` | Počet odpovědí se statusem 5xx — atomický čítač |

### `atomic.Int64` — proč ne `int64` s mutexem

`RequestCount` a `ErrorCount` se inkrementují z každé goroutiny obsluhy HTTP požadavku — potenciálně stovky souběžných goroutin zároveň. Klasický `int64` s mutexem by fungoval, ale zbytečně:

```go
// S mutexem — drahé
mu.Lock()
entry.RequestCount++
mu.Unlock()

// Atomic — levné, lock-free
entry.RequestCount.Add(1)
```

`atomic.Int64` používá hardware instrukci `LOCK XADD` (na x86) nebo ekvivalentní instrukci na ARM. Tato instrukce garantuje atomicitu na úrovni CPU — žádné zamykání, žádný context switch. Interně jde o **Compare-And-Swap (CAS)**: CPU přečte hodnotu, přičte k ní delta, zapíše zpět — a celá tato sekvence je nedělitelná. Pokud dvě jádra zkusí zapsat zároveň, jedno uspěje a druhé se automaticky opakuje. Výsledkem je konzistentní hodnota bez jediného mutexu.

### `json:"-"` tag

```go
RequestCount atomic.Int64 `json:"-"`
ErrorCount   atomic.Int64 `json:"-"`
```

Tag `json:"-"` říká standardnímu JSON encoderu: **toto pole přeskoc**. Důvody jsou dva:

1. `atomic.Int64` není plain hodnota — jde o struct s interním stavem, který `encoding/json` nedokáže bezpečně serializovat (přistupuje k poli přímo, nikoli přes metodu `.Load()`).
2. I kdyby to technicky šlo, hodnoty se exponují zvlášť přes `/stats` endpoint pomocí `.Load()`, který zajistí správné atomické čtení.

### Proč `RegisteredAt` a `LastChecked` zvlášť

`RegisteredAt` se nastaví jednou při registraci a nikdy se nemění — slouží k auditu ("tato služba je zaregistrovaná od X"). `LastChecked` se aktualizuje při každém health checku — říká "naposledy jsem viděl službu nahoře v Y". Jsou to dvě různé věci: jedna je identita záznamu, druhá je živý stav.

---

## Gateway struct

```go
type Gateway struct {
    mu       sync.RWMutex
    services map[string]*ServiceEntry
    proxies  map[string]*httputil.ReverseProxy

    edgeMu sync.RWMutex
    edges  []TopologyEdge

    startTime time.Time
}
```

### Proč dva oddělené `RWMutex`

`mu` chrání `services` a `proxies` — tato dvě pole se vždy mění společně (při registraci vznikne zároveň ServiceEntry i ReverseProxy, při odregistraci zaniknou oba). Proto sdílejí jeden mutex.

`edgeMu` chrání `edges` — topologie se mění nezávisle na registraci služeb. Hrana může být přidána kdykoli, bez jakékoli změny v `services`. Kdyby `edges` sdílely `mu` se `services`, čtení topologie by blokovalo registrace a naopak — zcela zbytečně.

`sync.RWMutex` navíc umožňuje **souběžné čtení**: metoda `RLock()` neblokuje ostatní čtenáře, pouze zapisovatele. Oddělené mutexy tedy maximalizují paralelismus — více goroutin může číst services i edges zároveň.

### `map[string]*ServiceEntry` — pointer, ne value

```go
services map[string]*ServiceEntry  // správně: pointer
// services map[string]ServiceEntry  // špatně: kopie
```

Mapa uchovává **ukazatele** na ServiceEntry, nikoli jejich kopie. To je zásadní ze dvou důvodů:

1. `atomic.Int64` uvnitř ServiceEntry se musí sdílet mezi handlerem (který inkrementuje čítač) a health checkerem (který čte stav). Kopie by měla vlastní čítač a sdílení by bylo iluzorní.
2. Update health stavu (`s.Status = "healthy"`) musí být viditelný pro všechny goroutiny, které drží ukazatel. Kopie by změnu izolovala.

### `map[string]*httputil.ReverseProxy` — proč cachovat proxy objekt

`httputil.ReverseProxy` není levný k vytvoření — inicializuje transport, nastaví Director funkci a alokuje interní struktury. Zároveň je navržen pro opakované použití a je goroutine-safe. Cachujeme ho v mapě při registraci a při každém příchozím požadavku ho jen vyhledáme — žádná alokace na hot path.

---

## register() a ReverseProxy

```go
func (g *Gateway) register(entry *ServiceEntry) error {
    target, err := url.Parse(entry.URL)
    if err != nil {
        return fmt.Errorf("invalid URL %q: %w", entry.URL, err)
    }

    proxy := httputil.NewSingleHostReverseProxy(target)

    origDirector := proxy.Director
    proxy.Director = func(req *http.Request) {
        origDirector(req)
        req.Host = target.Host
    }

    g.mu.Lock()
    g.services[entry.Name] = entry
    g.proxies[entry.Name] = proxy
    g.mu.Unlock()

    g.edgeMu.Lock()
    g.edges = append(g.edges, TopologyEdge{From: "gateway-go", To: entry.Name})
    g.edgeMu.Unlock()

    return nil
}
```

### `httputil.NewSingleHostReverseProxy` — co dělá

Vytvoří `http.Handler`, který:

1. Přepíše `Scheme` a `Host` v URL požadavku na cílový backend.
2. Přepošle požadavek (včetně těla a hlaviček) na backend.
3. Zkopíruje odpověď backendu zpět ke klientovi.

Veškerá logika HTTP tunelování — chunked encoding, WebSocket upgrade, error handling — je hotová a otestovaná.

### Proč override Director: nastavit `req.Host`

Výchozí Director z `NewSingleHostReverseProxy` přepíše `req.URL.Scheme` a `req.URL.Host`, ale ponechá `req.Host` header z původního požadavku klienta (např. `localhost:9000`). Některé backendové frameworky (Django, FastAPI, Rails) validují `Host` header a odmítnou požadavek, pokud se neshoduje s jejich konfigurovanou doménou.

Override Director proto nejprve zavolá původní logiku a pak přepíše `req.Host` na skutečný hostname backendu:

```go
origDirector := proxy.Director
proxy.Director = func(req *http.Request) {
    origDirector(req)          // přepíše Scheme, Host v URL
    req.Host = target.Host     // přepíše Host header na backend hostname
}
```

Proč zachováme `origDirector`? Abychom nepřepsali jeho logiku — přepisuje `req.URL`, nastavuje `X-Forwarded-For` a další hlavičky. Chceme tu logiku, jen k ní přidáme jeden řádek.

### Proč přidat TopologyEdge při registraci

Každá registrace automaticky přidá hranu `gateway-go → <service>`. Topologie tak vždy odráží fakt, že gateway je přímým volajícím každé registrované služby — bez nutnosti ručně tyto hrany přidávat.

!!! note "Parsování a alokace před zámkem"
    `url.Parse` a sestavení proxy objektu probíhají **před** zamčením `g.mu`. Zámek drží gateway co nejkratší dobu — pouze samotné zápisy do map — aby ostatní goroutiny (zejména proxy handlery) nebyly zbytečně blokovány.

---

## HTTP handlery — projdi každý

### `handleRegister`

```go
func (g *Gateway) handleRegister(w http.ResponseWriter, r *http.Request) {
    if r.Method != http.MethodPost {
        http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
        return
    }

    var entry ServiceEntry
    if err := json.NewDecoder(r.Body).Decode(&entry); err != nil {
        http.Error(w, "invalid JSON: "+err.Error(), http.StatusBadRequest)
        return
    }

    if entry.Name == "" || entry.URL == "" {
        http.Error(w, "name and url are required", http.StatusBadRequest)
        return
    }
    if entry.HealthPath == "" {
        entry.HealthPath = "/health"
    }
    entry.Status = "unknown"
    entry.RegisteredAt = time.Now()

    if err := g.register(&entry); err != nil {
        http.Error(w, err.Error(), http.StatusBadRequest)
        return
    }

    w.Header().Set("Content-Type", "application/json")
    w.WriteHeader(http.StatusCreated)
    json.NewEncoder(w).Encode(map[string]string{"status": "registered", "name": entry.Name})
}
```

Postup: ověření metody → JSON decode → validace povinných polí → nastavení výchozích hodnot → volání `register()` → odpověď `201 Created`.

Status se nastavuje na `"unknown"` protože health checker ještě neprovedl první kontrolu. `RegisteredAt` se nastaví zde, nikoli v `register()`, protože `register()` může být voláno i z jiných míst v budoucnu.

---

### `handleListServices`

```go
func (g *Gateway) handleListServices(w http.ResponseWriter, r *http.Request) {
    g.mu.RLock()
    list := make([]*ServiceEntry, 0, len(g.services))
    for _, svc := range g.services {
        list = append(list, svc)
    }
    g.mu.RUnlock()

    type serviceView struct {
        Name         string    `json:"name"`
        URL          string    `json:"url"`
        HealthPath   string    `json:"health_path"`
        Routes       []string  `json:"routes"`
        Status       string    `json:"status"`
        LatencyMs    int64     `json:"latency_ms"`
        LastChecked  time.Time `json:"last_checked"`
        RegisteredAt time.Time `json:"registered_at"`
        RequestCount int64     `json:"request_count"`
        ErrorCount   int64     `json:"error_count"`
    }

    views := make([]serviceView, 0, len(list))
    for _, svc := range list {
        views = append(views, serviceView{
            // ...
            RequestCount: svc.RequestCount.Load(),
            ErrorCount:   svc.ErrorCount.Load(),
        })
    }

    w.Header().Set("Content-Type", "application/json")
    json.NewEncoder(w).Encode(views)
}
```

**Proč `serviceView` struct místo přímé serializace `ServiceEntry`?**

`ServiceEntry` obsahuje `atomic.Int64`, která nemá JSON tag (má `json:"-"`). Přímá serializace by tato pole vynechala. Lokální `serviceView` struct slouží jako **DTO (Data Transfer Object)** — má plain `int64` pro `RequestCount` a `ErrorCount`, které se naplní voláním `.Load()`. Zároveň umožňuje naplnit snapshot atomických hodnot **po** uvolnění RLock, bez rizika race condition.

---

### `handleDeregister`

```go
func (g *Gateway) handleDeregister(w http.ResponseWriter, r *http.Request) {
    name := strings.TrimPrefix(r.URL.Path, "/services/")
    name = strings.Trim(name, "/")
    if name == "" {
        http.Error(w, "service name required", http.StatusBadRequest)
        return
    }

    if !g.deregister(name) {
        http.Error(w, "service not found", http.StatusNotFound)
        return
    }

    w.Header().Set("Content-Type", "application/json")
    json.NewEncoder(w).Encode(map[string]string{"status": "deregistered", "name": name})
}
```

`strings.TrimPrefix` odstraní `/services/` ze začátku cesty a vrátí název služby. Druhý `strings.Trim` odstraní případné trailing lomítko — `DELETE /services/contacts/` a `DELETE /services/contacts` jsou ekvivalentní. Pokud `deregister()` vrátí `false`, služba neexistovala → odpověď `404`.

---

### `handleStats`

```go
func (g *Gateway) handleStats(w http.ResponseWriter, r *http.Request) {
    g.mu.RLock()
    stats := make(map[string]map[string]int64, len(g.services))
    for name, svc := range g.services {
        stats[name] = map[string]int64{
            "request_count": svc.RequestCount.Load(),
            "error_count":   svc.ErrorCount.Load(),
        }
    }
    g.mu.RUnlock()

    w.Header().Set("Content-Type", "application/json")
    json.NewEncoder(w).Encode(stats)
}
```

Handler čte pod `RLock` — ostatní čtenáři mohou číst zároveň. `.Load()` je atomické a bezpečné i pod RLock, protože atomic operace nepotřebují vlastní mutex. Výsledkem je mapa `{ "contacts-cpp": { "request_count": 42, "error_count": 1 }, ... }`.

---

### `handleTopology` a `handleAddEdge`

```go
func (g *Gateway) handleTopology(w http.ResponseWriter, r *http.Request) {
    g.edgeMu.RLock()
    edges := make([]TopologyEdge, len(g.edges))
    copy(edges, g.edges)
    g.edgeMu.RUnlock()

    w.Header().Set("Content-Type", "application/json")
    json.NewEncoder(w).Encode(map[string]any{"edges": edges})
}
```

```go
func (g *Gateway) handleAddEdge(w http.ResponseWriter, r *http.Request) {
    var edge TopologyEdge
    if err := json.NewDecoder(r.Body).Decode(&edge); err != nil {
        http.Error(w, "invalid JSON: "+err.Error(), http.StatusBadRequest)
        return
    }
    if edge.From == "" || edge.To == "" {
        http.Error(w, "from and to are required", http.StatusBadRequest)
        return
    }

    g.edgeMu.Lock()
    g.edges = append(g.edges, edge)
    g.edgeMu.Unlock()

    w.WriteHeader(http.StatusCreated)
    json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}
```

`handleTopology` zkopíruje slice pod zámkem (`copy`) — serializace pak probíhá bez zámku, bez rizika race condition při souběžném přidávání hran. `handleAddEdge` umožňuje službám registrovat vlastní hrany (např. `bff-python → contacts-cpp`), čímž se topologie stává kompletním grafem závislostí.

---

### `handleProxy` — detailně

```go
func (g *Gateway) handleProxy(w http.ResponseWriter, r *http.Request) {
    parts := strings.SplitN(strings.TrimPrefix(r.URL.Path, "/"), "/", 2)
    serviceName := parts[0]
    if serviceName == "" {
        http.Error(w, "missing service name in path", http.StatusNotFound)
        return
    }

    g.mu.RLock()
    svc, ok := g.services[serviceName]
    proxy, pok := g.proxies[serviceName]
    g.mu.RUnlock()

    if !ok || !pok {
        http.Error(w, fmt.Sprintf("service %q not registered", serviceName), http.StatusNotFound)
        return
    }

    if len(parts) > 1 {
        r.URL.Path = "/" + parts[1]
    } else {
        r.URL.Path = "/"
    }
    if r.URL.RawPath != "" {
        rawParts := strings.SplitN(strings.TrimPrefix(r.URL.RawPath, "/"), "/", 2)
        if len(rawParts) > 1 {
            r.URL.RawPath = "/" + rawParts[1]
        } else {
            r.URL.RawPath = "/"
        }
    }

    svc.RequestCount.Add(1)

    rw := &statusRecorder{ResponseWriter: w, code: http.StatusOK}
    proxy.ServeHTTP(rw, r)

    if rw.code >= 500 {
        svc.ErrorCount.Add(1)
    }
}
```

**`strings.SplitN` s limitem 2:**

```
Vstup: "/contacts-cpp/api/users/123"
TrimPrefix: "contacts-cpp/api/users/123"
SplitN(..., 2): ["contacts-cpp", "api/users/123"]
```

Limit `2` zajistí, že zbytek cesty za prvním lomítkem zůstane celý v `parts[1]` — nevznikají zbytečné alokace pro dlouhé cesty a backend dostane cestu přesně tak, jak ji klient poslal.

**Path stripping:**

```
Klient volá:   GET /contacts-cpp/api/users/123
Gateway předá: GET /api/users/123  →  http://contacts-cpp:8080
```

**`RawPath` edge case:**

`r.URL.Path` je dekódovaná verze cesty (např. `/files/my file.txt`). `r.URL.RawPath` je percent-encoded verze (`/files/my%20file.txt`) a nastaví se pouze pokud se liší od `Path`. `httputil.ReverseProxy` preferuje `RawPath` pokud je nastaveno — proto musíme stripovat prefix i tam. Kdybychom to neudělali, backend by viděl `/contacts-cpp/files/my%20file.txt` místo `/files/my%20file.txt`.

**`statusRecorder`:**

`httputil.ReverseProxy` volá `WriteHeader` interně — bez wrapperu bychom nikdy nezjistili status kód odpovědi backendu. `statusRecorder` ho zachytí a umožní nám inkrementovat `ErrorCount` při 5xx.

**Proč jen 5xx, ne 4xx?**

5xx jsou chyby backendu — backend selhal při zpracování. 4xx jsou chyby klienta (špatný požadavek, chybějící autorizace) — backend funguje správně, jen klient poslal neplatný požadavek. Počítáme problémy služby, ne problémy klientů.

---

## statusRecorder

```go
type statusRecorder struct {
    http.ResponseWriter
    code    int
    written bool
}

func (sr *statusRecorder) WriteHeader(code int) {
    if !sr.written {
        sr.code = code
        sr.written = true
    }
    sr.ResponseWriter.WriteHeader(code)
}
```

`statusRecorder` embedduje `http.ResponseWriter` — jde o **struct embedding rozhraní**. To znamená, že všechny metody `ResponseWriter` (`Write`, `Header`, atd.) jsou automaticky delegovány na vložené rozhraní. Nepíšeme forwarding metody ručně — Go to udělá za nás. Přepišujeme pouze `WriteHeader`.

**`written bool` guard:**

HTTP protokol neumožňuje po odeslání hlaviček změnit status kód. `httputil.ReverseProxy` může za určitých okolností volat `WriteHeader` vícekrát (např. při přepisu chybové odpovědi backendu). Guard `written` zajistí, že zachytíme pouze první volání — to, které skutečně určí status kód odpovědi.

!!! warning "Embedding interface vs embedding struct"
    Pokud by `statusRecorder` embeddoval konkrétní typ (ne rozhraní), museli bychom ho přebírat jako pointer na konkrétní implementaci ResponseWriter. Embedding rozhraní umožňuje zabalit libovolnou implementaci ResponseWriter — gateway tak funguje s jakýmkoli HTTP serverem, nejen se standardním.

---

## runHealthChecker

```go
func (g *Gateway) runHealthChecker() {
    ticker := time.NewTicker(10 * time.Second)
    defer ticker.Stop()

    client := &http.Client{Timeout: 5 * time.Second}

    for range ticker.C {
        g.mu.RLock()
        names := make([]string, 0, len(g.services))
        for name := range g.services {
            names = append(names, name)
        }
        g.mu.RUnlock()

        for _, name := range names {
            go g.checkHealth(client, name)
        }
    }
}
```

### `time.NewTicker` vs `time.Sleep` — proč ticker

```go
// time.Sleep — driftuje
for {
    checkAll()
    time.Sleep(10 * time.Second)  // interval = 10s + doba trvání checkAll()
}

// time.NewTicker — drift-free
ticker := time.NewTicker(10 * time.Second)
for range ticker.C {
    go checkAll()  // ticker tik každých přesně 10s, bez ohledu na dobu trvání
}
```

`time.Sleep` přidá prodlevu **za** provedenou prací — reálný interval je `10s + doba trvání health checků`. U pěti pomalých backendů s timeoutem 5s by interval byl `10s + 5s = 15s`. `time.NewTicker` tick každých přesně 10 sekund od posledního tiku, nezávisle na době zpracování. Výsledkem je přibližně konstantní interval i při proměnlivé latenci backendů.

### Goroutina per service — proč ne sekvenčně

```go
for _, name := range names {
    go g.checkHealth(client, name)  // paralelně
}
```

Bez `go` by N nedostupných služeb s timeoutem 5s znamenalo `N × 5s` čekání — 10 služeb = 50s na jeden cyklus, přičemž cyklus se opakuje každých 10s. Se samostatnou goroutinou na každou službu probíhají všechny health checky paralelně a celý cyklus trvá maximálně 5s (timeout jednoho checku).

### Proč lock při update, ačkoli `atomic.Int64` nepotřebuje lock

V `checkHealth` se pod write lockem aktualizují tři pole najednou:

```go
g.mu.Lock()
if s, exists := g.services[name]; exists {
    s.Status = status
    s.LatencyMs = latency
    s.LastChecked = time.Now()
}
g.mu.Unlock()
```

`Status`, `LatencyMs` a `LastChecked` jsou plain `string`, `int64` a `time.Time` — nejsou atomic. Čtenář (např. `handleListServices`) musí vidět konzistentní trojici — nikdy ne nové `LatencyMs` se starým `Status`. Write lock garantuje, že čtenáři vždy vidí atomický update všech tří polí najednou.

---

## buildMux a main()

```go
func (g *Gateway) buildMux() http.Handler {
    mux := http.NewServeMux()

    mux.HandleFunc("/health", g.handleHealth)
    mux.HandleFunc("/stats", g.handleStats)
    mux.HandleFunc("/topology/edge", g.handleAddEdge)
    mux.HandleFunc("/topology", g.handleTopology)
    mux.HandleFunc("/services", func(w http.ResponseWriter, r *http.Request) {
        switch r.Method {
        case http.MethodPost:
            g.handleRegister(w, r)
        case http.MethodGet:
            g.handleListServices(w, r)
        default:
            http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
        }
    })
    mux.HandleFunc("/services/", g.handleDeregister)

    mux.HandleFunc("/", g.handleProxy)

    return mux
}
```

### Proč specifické routes před catch-all `/`

`http.ServeMux` používá **longest-prefix matching** — delší (specifičtější) prefix má vždy přednost před kratším. Registrování `/health` zajistí, že `GET /health` nikdy nedorazí do `handleProxy`.

!!! note "Trailing slash v ServeMux"
    `/services` (bez lomítka) matchuje pouze přesně `/services`. `/services/` (s lomítkem) matchuje `/services/` i jakékoli `/services/cokoliv`. Proto jsou potřeba oba — `/services` pro `POST`/`GET` a `/services/` pro `DELETE /services/{name}`.

```go
func main() {
    gw := newGateway()
    go gw.runHealthChecker()

    srv := &http.Server{
        Addr:         "0.0.0.0:9000",
        Handler:      gw.buildMux(),
        ReadTimeout:  30 * time.Second,
        WriteTimeout: 60 * time.Second,
        IdleTimeout:  120 * time.Second,
    }

    if err := srv.ListenAndServe(); err != nil {
        log.Fatalf("server error: %v", err)
    }
}
```

### Timeouty — co každý chrání

| Timeout | Hodnota | Co chrání |
|---|---|---|
| `ReadTimeout` | 30s | Čas na přečtení celého požadavku (hlavičky + tělo). Chrání před **slowloris** — útočník posílá data velmi pomalu a blokuje goroutinu navždy. |
| `WriteTimeout` | 60s | Čas na odeslání celé odpovědi. Delší než ReadTimeout, protože proxy odpovědi mohou být objemné a backend potřebuje čas na sestavení. |
| `IdleTimeout` | 120s | Čas na přijetí dalšího požadavku na keep-alive spojení. Uvolní goroutinu po nečinném spojení — zabrání hromadění idle klientů. |

`WriteTimeout` je záměrně 2× delší než `ReadTimeout` — při proxying může backend generovat velké odpovědi (export vCard, DB dump) a klient je stahuje pomalu. Příliš krátký `WriteTimeout` by tyto legitimní požadavky přerušil.
