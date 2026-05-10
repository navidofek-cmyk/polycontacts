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
type ServiceEntry struct {
	Name         string    `json:"name"`
	URL          string    `json:"url"`
	HealthPath   string    `json:"health_path"`
	Routes       []string  `json:"routes"`
	Status       string    `json:"status"`        // "healthy" | "unhealthy" | "unknown"
	LatencyMs    int64     `json:"latency_ms"`
	LastChecked  time.Time `json:"last_checked"`
	RegisteredAt time.Time `json:"registered_at"`

	// Atomic counters — not serialised directly; exposed via /stats.
	RequestCount atomic.Int64 `json:"-"`
	ErrorCount   atomic.Int64 `json:"-"`
}

// TopologyEdge represents a directed call edge between two services.
type TopologyEdge struct {
	From string `json:"from"`
	To   string `json:"to"`
}

// Gateway is the central state of the API gateway.
type Gateway struct {
	mu       sync.RWMutex
	services map[string]*ServiceEntry
	proxies  map[string]*httputil.ReverseProxy

	edgeMu sync.RWMutex
	edges  []TopologyEdge

	startTime time.Time
}

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

func (g *Gateway) register(entry *ServiceEntry) error {
	target, err := url.Parse(entry.URL)
	if err != nil {
		return fmt.Errorf("invalid URL %q: %w", entry.URL, err)
	}

	proxy := httputil.NewSingleHostReverseProxy(target)

	// Wrap the default director so we strip the /<service-name> prefix before
	// forwarding and set a sensible Host header.
	origDirector := proxy.Director
	proxy.Director = func(req *http.Request) {
		origDirector(req)
		req.Host = target.Host
	}

	g.mu.Lock()
	g.services[entry.Name] = entry
	g.proxies[entry.Name] = proxy
	g.mu.Unlock()

	// Add gateway→service edge.
	g.edgeMu.Lock()
	g.edges = append(g.edges, TopologyEdge{From: "gateway-go", To: entry.Name})
	g.edgeMu.Unlock()

	return nil
}

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

// POST /services
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

	log.Printf("registered service %q at %s (routes: %v)", entry.Name, entry.URL, entry.Routes)

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusCreated)
	json.NewEncoder(w).Encode(map[string]string{"status": "registered", "name": entry.Name})
}

// GET /services
func (g *Gateway) handleListServices(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	g.mu.RLock()
	list := make([]*ServiceEntry, 0, len(g.services))
	for _, svc := range g.services {
		list = append(list, svc)
	}
	g.mu.RUnlock()

	// Build a JSON-friendly representation that includes the atomic counters.
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
			RequestCount: svc.RequestCount.Load(),
			ErrorCount:   svc.ErrorCount.Load(),
		})
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(views)
}

// DELETE /services/{name}
func (g *Gateway) handleDeregister(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodDelete {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

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

// GET /health
func (g *Gateway) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	uptime := int64(time.Since(g.startTime).Seconds())
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{
		"status":    "ok",
		"service":   "gateway-go",
		"uptime_s":  uptime,
	})
}

// GET /stats
func (g *Gateway) handleStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

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

// GET /topology
func (g *Gateway) handleTopology(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	g.edgeMu.RLock()
	edges := make([]TopologyEdge, len(g.edges))
	copy(edges, g.edges)
	g.edgeMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(map[string]any{"edges": edges})
}

// POST /topology/edge
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

// Catch-all proxy handler: /<service-name>/rest/of/path
func (g *Gateway) handleProxy(w http.ResponseWriter, r *http.Request) {
	// Extract the first path segment as the service name.
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

	// Strip the leading /<service-name> prefix so the backend sees /rest/of/path.
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

	// Wrap the ResponseWriter to capture status code for error counting.
	rw := &statusRecorder{ResponseWriter: w, code: http.StatusOK}
	proxy.ServeHTTP(rw, r)

	if rw.code >= 500 {
		svc.ErrorCount.Add(1)
	}
}

// -------------------------------------------------------------------------
// Status recording ResponseWriter
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
// Background health checker
// -------------------------------------------------------------------------

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

func (g *Gateway) checkHealth(client *http.Client, name string) {
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

	status := "unhealthy"
	if err == nil {
		resp.Body.Close()
		if resp.StatusCode < 500 {
			status = "healthy"
		}
	}

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

func (g *Gateway) buildMux() http.Handler {
	mux := http.NewServeMux()

	// Specific routes take priority.
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

	// Catch-all proxy.
	mux.HandleFunc("/", g.handleProxy)

	return mux
}

func main() {
	gw := newGateway()
	go gw.runHealthChecker()

	addr := "0.0.0.0:9000"
	log.Printf("gateway-go listening on %s", addr)

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
