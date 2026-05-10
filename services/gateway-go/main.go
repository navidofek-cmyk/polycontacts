package main

import (
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"net/http/httputil"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// ServiceEntry holds registration info and live stats for a backend service.
// Atomic counters (RequestCount, ErrorCount) jsou oddělené od ostatních polí záměrně —
// umožňují bezpečný přístup z více goroutin bez nutnosti zamykat celou strukturu.
type ServiceEntry struct {
	Name         string    `json:"name"`
	URL          string    `json:"url"`
	HealthPath   string    `json:"health_path"`
	Routes       []string  `json:"routes"`
	Status       string    `json:"status"`        // "healthy" | "unhealthy" | "unknown"
	LatencyMs    int64     `json:"latency_ms"`
	LastChecked  time.Time `json:"last_checked"`
	RegisteredAt time.Time `json:"registered_at"`

	// atomic.Int64 místo obyčejného int64 + mutex: každý požadavek inkrementuje
	// čítač z jiné goroutiny. atomic operace jsou lock-free — hardware instrukce
	// (např. LOCK XADD na x86) garantují konzistenci bez režie mutexu.
	// json:"-" zabraňuje přímé serializaci, protože atomic.Int64 nelze bezpečně
	// zakódovat do JSON; hodnoty se exponují přes /stats endpoint zvlášť.
	RequestCount atomic.Int64 `json:"-"`
	ErrorCount   atomic.Int64 `json:"-"`
}

// TopologyEdge represents a directed call edge between two services.
// Slouží k vizualizaci závislostí — kdo volá koho — v /topology endpointu.
type TopologyEdge struct {
	From string `json:"from"`
	To   string `json:"to"`
}

// Gateway is the central state of the API gateway.
// Drží registry služeb, jejich reverzní proxy a topologii volání.
//
// Proč dva oddělené mutexy (mu a edgeMu)?
// services/proxies a edges jsou nezávislé datové struktury s různou četností
// přístupu. Jediný mutex by zbytečně blokoval čtení topologie při registraci
// služby (a naopak). Separátní RWMutex pro každou strukturu maximalizuje
// souběžnost čtení — více goroutin může číst obě struktury zároveň.
type Gateway struct {
	// mu chrání services a proxies — tato dvě pole se vždy mění společně
	// (při registraci/odregistraci), proto sdílejí jeden mutex.
	mu       sync.RWMutex
	services map[string]*ServiceEntry
	proxies  map[string]*httputil.ReverseProxy

	// edgeMu je záměrně oddělený od mu: topologie se mění nezávisle na
	// registraci služeb (edge lze přidat i mezi již registrované služby).
	edgeMu sync.RWMutex
	edges  []TopologyEdge

	startTime time.Time
}

// newGateway creates an empty Gateway ready to accept service registrations.
func newGateway() *Gateway {
	return &Gateway{
		services:  make(map[string]*ServiceEntry),
		proxies:   make(map[string]*httputil.ReverseProxy),
		startTime: time.Now(),
	}
}

// -------------------------------------------------------------------------
// Registration helpers
// -------------------------------------------------------------------------

// register adds a service to the gateway and creates its reverse proxy.
// Volající předává ukazatel; Gateway si interně uchovává stejný ukazatel,
// takže atomic čítače v ServiceEntry jsou sdílené a nepotřebují kopírování.
func (g *Gateway) register(entry *ServiceEntry) error {
	target, err := url.Parse(entry.URL)
	if err != nil {
		return fmt.Errorf("invalid URL %q: %w", entry.URL, err)
	}

	proxy := httputil.NewSingleHostReverseProxy(target)

	// Výchozí Director z NewSingleHostReverseProxy správně přepíše Scheme a Host
	// cílové URL, ale ponechá Host header z původního požadavku klienta.
	// Overridujeme ho tak, aby backend viděl svůj vlastní hostname — bez toho
	// některé frameworky odmítají požadavek jako nevalidní (virtual hosting).
	origDirector := proxy.Director
	proxy.Director = func(req *http.Request) {
		// Nejprve zavolej původní logiku (přepis Scheme, Host, Path na cíl).
		origDirector(req)
		// Pak přepis Host header na skutečný hostname backendu, nikoli klienta.
		req.Host = target.Host
	}

	// Zámek drž co nejkratší dobu — parsování URL a sestavení proxy probíhá
	// před zamčením, aby ostatní goroutiny nebyly blokovány zbytečně.
	g.mu.Lock()
	g.services[entry.Name] = entry
	g.proxies[entry.Name] = proxy
	g.mu.Unlock()

	// Automaticky zaznamenáme hranu gateway→service, aby topologie odrážela
	// fakt, že gateway je přímým volajícím každé registrované služby.
	g.edgeMu.Lock()
	g.edges = append(g.edges, TopologyEdge{From: "gateway-go", To: entry.Name})
	g.edgeMu.Unlock()

	return nil
}

// deregister removes a service and its proxy from the gateway.
// Vrací false, pokud služba neexistuje — volající může rozlišit 404 od 200.
func (g *Gateway) deregister(name string) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	if _, ok := g.services[name]; !ok {
		return false
	}
	delete(g.services, name)
	delete(g.proxies, name)
	return true
}

// -------------------------------------------------------------------------
// HTTP handlers
// -------------------------------------------------------------------------

// handleRegister handles POST /services — registers a new backend service.
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
	// Rozumná výchozí hodnota — většina Go/Node/Python frameworků vystavuje
	// health endpoint právě na /health.
	if entry.HealthPath == "" {
		entry.HealthPath = "/health"
	}
	// Stav "unknown" signalizuje, že health checker ještě neprovedl první kontrolu.
	entry.Status = "unknown"
	entry.RegisteredAt = time.Now()

	if err := g.register(&entry); err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}

	log.Printf("registered service %q at %s (routes: %v)", entry.Name, entry.URL, entry.Routes)

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(map[string]string{"status": "registered", "name": entry.Name})
}

// handleListServices handles GET /services — returns all registered services with live counters.
func (g *Gateway) handleListServices(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// RLock (čtecí zámek) umožňuje více souběžným goroutinám číst zároveň;
	// blokuje pouze pokud právě probíhá zápis (registrace/odregistrace).
	g.mu.RLock()
	list := make([]*ServiceEntry, 0, len(g.services))
	for _, svc := range g.services {
		list = append(list, svc)
	}
	g.mu.RUnlock()
	// Zámek uvolníme co nejdříve — serializace do JSON může trvat déle a
	// nepotřebuje exkluzivní přístup k mapě.

	// Lokální anonymní struct místo přímé serializace ServiceEntry ze dvou důvodů:
	// 1. atomic.Int64 nelze přímo JSON-enkódovat (není to plain int64).
	// 2. Umožňuje načíst snapshot hodnot přes .Load() bez držení zámku.
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
			Name:         svc.Name,
			URL:          svc.URL,
			HealthPath:   svc.HealthPath,
			Routes:       svc.Routes,
			Status:       svc.Status,
			LatencyMs:    svc.LatencyMs,
			LastChecked:  svc.LastChecked,
			RegisteredAt: svc.RegisteredAt,
			// .Load() je atomic čtení — bezpečné bez zámku, viz komentář u deklarace.
			RequestCount: svc.RequestCount.Load(),
			ErrorCount:   svc.ErrorCount.Load(),
		})
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(views)
}

// handleDeregister handles DELETE /services/{name} — removes a service from the gateway.
func (g *Gateway) handleDeregister(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodDelete {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Cesta je /services/{name}; ořežeme prefix a případné lomítka.
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

	log.Printf("deregistered service %q", name)
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]string{"status": "deregistered", "name": name})
}

// handleHealth handles GET /health — reports gateway liveness and uptime.
// Tento endpoint je záměrně jednoduchý a nezamyká žádnou sdílenou strukturu,
// aby byl dostupný i při vysoké zátěži na registrace/proxy.
func (g *Gateway) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	uptime := int64(time.Since(g.startTime).Seconds())
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"status":   "ok",
		"service":  "gateway-go",
		"uptime_s": uptime,
	})
}

// handleStats handles GET /stats — returns per-service request and error counts.
func (g *Gateway) handleStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	g.mu.RLock()
	stats := make(map[string]map[string]int64, len(g.services))
	for name, svc := range g.services {
		stats[name] = map[string]int64{
			// .Load() je atomic — bezpečné přímo pod RLock, bez dalšího zamykání.
			"request_count": svc.RequestCount.Load(),
			"error_count":   svc.ErrorCount.Load(),
		}
	}
	g.mu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(stats)
}

// handleTopology handles GET /topology — returns the current service call graph.
func (g *Gateway) handleTopology(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	// Zkopírujeme slice pod zámkem, aby serializace mohla probíhat bez něj.
	g.edgeMu.RLock()
	edges := make([]TopologyEdge, len(g.edges))
	copy(edges, g.edges)
	g.edgeMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{"edges": edges})
}

// handleAddEdge handles POST /topology/edge — records a service-to-service call relationship.
// Umožňuje službám registrovat vlastní hrany (např. contacts-svc → mail-svc),
// čímž se topologie stává kompletním grafem závislostí celého systému.
func (g *Gateway) handleAddEdge(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

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

	log.Printf("topology edge added: %s → %s", edge.From, edge.To)

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
}

// handleProxy is the catch-all handler that forwards /<service>/... to the backend.
//
// Path stripping — proč a jak:
// Klient volá gateway jako /contacts/api/users. Gateway musí backendu předat
// jen /api/users, protože backend neví, že existuje gateway a prefix /contacts.
// Implementace: rozdělíme cestu na nejvýše 2 části podle prvního lomítka.
//   parts[0] = "contacts"   → název služby
//   parts[1] = "api/users"  → zbytek cesty pro backend
// Výsledná cesta pro backend: "/" + parts[1] = "/api/users".
// RawPath (percent-encoded verze) se ošetřuje stejně, aby nedošlo ke dvojitému
// dekódování escape sekvencí (např. %2F → %252F).
func (g *Gateway) handleProxy(w http.ResponseWriter, r *http.Request) {
	// SplitN s limitem 2 zajistí, že zbytek cesty za prvním lomítkem zůstane
	// celý v parts[1] — nevznikají zbytečné alokace pro dlouhé cesty.
	parts := strings.SplitN(strings.TrimPrefix(r.URL.Path, "/"), "/", 2)
	serviceName := parts[0]
	if serviceName == "" {
		http.Error(w, "missing service name in path", http.StatusNotFound)
		return
	}

	// RLock stačí — pouze čteme z mapy, nepišeme.
	g.mu.RLock()
	svc, ok := g.services[serviceName]
	proxy, pok := g.proxies[serviceName]
	g.mu.RUnlock()

	if !ok || !pok {
		http.Error(w, fmt.Sprintf("service %q not registered", serviceName), http.StatusNotFound)
		return
	}

	// Přepis URL.Path: odstraníme prefix /<service-name> tak, aby backend
	// viděl cestu relativní ke svému root, nikoli ke gateway root.
	if len(parts) > 1 {
		r.URL.Path = "/" + parts[1]
	} else {
		// Klient volal přesně /<service-name> bez dalšího suffixu → backend dostane /.
		r.URL.Path = "/"
	}
	// RawPath je percent-encoded verze Path; pokud je nastavena, musíme ji
	// stripovat stejně, jinak httputil.ReverseProxy použije RawPath místo Path.
	if r.URL.RawPath != "" {
		rawParts := strings.SplitN(strings.TrimPrefix(r.URL.RawPath, "/"), "/", 2)
		if len(rawParts) > 1 {
			r.URL.RawPath = "/" + rawParts[1]
		} else {
			r.URL.RawPath = "/"
		}
	}

	// atomic.Add je bezpečné bez zámku — viz komentář u deklarace RequestCount.
	svc.RequestCount.Add(1)

	// statusRecorder obaluje ResponseWriter, aby zachytil HTTP status kód odpovědi.
	// httputil.ReverseProxy volá WriteHeader interně; bez wrapperu bychom status
	// kód nikdy nezjistili a nemohli inkrementovat ErrorCount.
	rw := &statusRecorder{ResponseWriter: w, code: http.StatusOK}
	proxy.ServeHTTP(rw, r)

	// Počítáme pouze 5xx jako chyby backendu; 4xx jsou chyby klienta, ne služby.
	if rw.code >= 500 {
		svc.ErrorCount.Add(1)
	}
}

// -------------------------------------------------------------------------
// Status recording ResponseWriter
// -------------------------------------------------------------------------

// statusRecorder wraps http.ResponseWriter to capture the HTTP status code
// written by the reverse proxy, without interfering with the response stream.
// Potřebujeme ho proto, že http.ResponseWriter standardně nenabízí způsob,
// jak zpětně zjistit, jaký status kód byl odeslán — WriteHeader je one-way.
type statusRecorder struct {
	http.ResponseWriter        // vložené rozhraní — delegujeme Write a Header automaticky
	code            int        // zachycený status kód; výchozí 200 (OK)
	written         bool       // guard proti dvojitému zachycení (proxy může volat WriteHeader vícekrát)
}

// WriteHeader intercepts the status code before passing it to the real ResponseWriter.
func (sr *statusRecorder) WriteHeader(code int) {
	// Zachytíme pouze první volání — HTTP protokol neumožňuje změnit status kód
	// po odeslání hlaviček, takže druhé a další volání jsou již bez efektu.
	if !sr.written {
		sr.code = code
		sr.written = true
	}
	sr.ResponseWriter.WriteHeader(code)
}

// -------------------------------------------------------------------------
// Background health checker
// -------------------------------------------------------------------------

// runHealthChecker periodically probes all registered services and updates their status.
// Běží jako dedikovaná goroutina spuštěná z main(); nikdy nevrátí, pokud
// není proces ukončen. Ticker je preferován před time.Sleep, protože zajišťuje
// přibližně konstantní interval bez ohledu na dobu trvání health checků.
func (g *Gateway) runHealthChecker() {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()

	// Sdílený HTTP klient s timeoutem pro všechna health check volání.
	// Timeout 5 s je kratší než interval 10 s, aby se kontroly nepřekrývaly
	// v případě pomalých nebo nedostupných backendů.
	client := &http.Client{Timeout: 5 * time.Second}

	for range ticker.C {
		// Pořídíme snapshot názvů pod RLock — nechceme držet zámek po dobu
		// síťových volání, která mohou trvat až 5 sekund.
		g.mu.RLock()
		names := make([]string, 0, len(g.services))
		for name := range g.services {
			names = append(names, name)
		}
		g.mu.RUnlock()

		// Každá služba dostane vlastní goroutinu, aby health checky probíhaly
		// paralelně. Bez toho by N pomalých/nedostupných služeb blokovalo
		// ostatní a celý cyklus by trvalo N×timeout sekund místo 1×timeout.
		for _, name := range names {
			go g.checkHealth(client, name)
		}
	}
}

// checkHealth probes the health endpoint of a single service and updates its status.
// Voláno jako goroutina z runHealthChecker — bezpečné pro souběžné spuštění
// pro více služeb zároveň díky oddělené synchronizaci přes g.mu.
func (g *Gateway) checkHealth(client *http.Client, name string) {
	// Nejprve ověříme, že služba stále existuje — mohla být odregistrována
	// mezi sestavením snapshotu a spuštěním této goroutiny.
	g.mu.RLock()
	svc, ok := g.services[name]
	g.mu.RUnlock()
	if !ok {
		return
	}

	healthURL := strings.TrimRight(svc.URL, "/") + svc.HealthPath

	start := time.Now()
	resp, err := client.Get(healthURL)
	latency := time.Since(start).Milliseconds()

	// Jakýkoliv 2xx nebo 4xx status považujeme za "healthy" — service je živá
	// a odpovídá. 5xx nebo síťová chyba znamená, že backend nefunguje správně.
	status := "unhealthy"
	if err == nil {
		resp.Body.Close()
		if resp.StatusCode < 500 {
			status = "healthy"
		}
	}

	// Zápis pod write lockem — aktualizujeme tři pole najednou, aby byl stav
	// vždy konzistentní (čtenář nikdy neuvidí nové latency se starým statusem).
	g.mu.Lock()
	if s, exists := g.services[name]; exists {
		s.Status = status
		s.LatencyMs = latency
		s.LastChecked = time.Now()
	}
	g.mu.Unlock()

	log.Printf("health-check %s → %s (%dms)", name, status, latency)
}

// -------------------------------------------------------------------------
// Router + main
// -------------------------------------------------------------------------

// buildMux registers all gateway routes and returns the configured HTTP handler.
// Specifičtější cesty (s delším prefixem) musí být registrovány před catch-all
// "/" — Go's http.ServeMux provádí longest-prefix matching, takže /health
// bude mít přednost před /, ale pouze pokud je registrováno explicitně.
func (g *Gateway) buildMux() http.Handler {
	mux := http.NewServeMux()

	// Management endpointy registrujeme před catch-all proxy ("/"),
	// aby nikdy nebyly omylem přesměrovány na backend službu.
	mux.HandleFunc("/health", g.handleHealth)
	mux.HandleFunc("/stats", g.handleStats)
	mux.HandleFunc("/topology/edge", g.handleAddEdge)
	mux.HandleFunc("/topology", g.handleTopology)
	// /services obsluhuje jak POST (registrace), tak GET (seznam) na stejné cestě.
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
	// /services/ (s lomítkem) zachytí /services/{name} pro DELETE.
	mux.HandleFunc("/services/", g.handleDeregister)

	// Catch-all — vše ostatní jde do reverzní proxy; prefix /<service-name>
	// určuje, na který backend požadavek přeposlat.
	mux.HandleFunc("/", g.handleProxy)

	return mux
}

func main() {
	gw := newGateway()
	// Health checker spustíme jako background goroutinu — běží souběžně
	// s HTTP serverem po celou dobu životnosti procesu.
	go gw.runHealthChecker()

	addr := "0.0.0.0:9000"
	log.Printf("gateway-go listening on %s", addr)

	// Explicitní timeouty chrání před pomalými klienty (slowloris apod.).
	// WriteTimeout je delší než ReadTimeout, protože proxy odpovědi mohou
	// být objemné a backend potřebuje čas na jejich sestavení.
	srv := &http.Server{
		Addr:         addr,
		Handler:      gw.buildMux(),
		ReadTimeout:  30 * time.Second,
		WriteTimeout: 60 * time.Second,
		IdleTimeout:  120 * time.Second,
	}

	if err := srv.ListenAndServe(); err != nil {
		log.Fatalf("server error: %v", err)
	}
}
