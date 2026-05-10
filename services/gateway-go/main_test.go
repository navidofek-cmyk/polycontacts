package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// newTestGateway creates a fresh Gateway and returns it together with its mux.
func newTestGateway() (*Gateway, http.Handler) {
	gw := newGateway()
	return gw, gw.buildMux()
}

// registerService is a helper that POSTs a service registration JSON and
// returns the recorded response.
func registerService(t *testing.T, mux http.Handler, name, rawURL string) *httptest.ResponseRecorder {
	t.Helper()
	body := strings.NewReader(`{"name":"` + name + `","url":"` + rawURL + `"}`)
	req := httptest.NewRequest(http.MethodPost, "/services", body)
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req)
	return rr
}

// ---------------------------------------------------------------------------
// TestHandleHealth
// ---------------------------------------------------------------------------

func TestHandleHealth(t *testing.T) {
	_, mux := newTestGateway()

	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rr.Code)
	}

	var body map[string]any
	if err := json.NewDecoder(rr.Body).Decode(&body); err != nil {
		t.Fatalf("failed to decode JSON: %v", err)
	}

	if body["status"] != "ok" {
		t.Errorf("expected status=ok, got %v", body["status"])
	}
}

// ---------------------------------------------------------------------------
// TestHandleRegister
// ---------------------------------------------------------------------------

func TestHandleRegister(t *testing.T) {
	_, mux := newTestGateway()

	rr := registerService(t, mux, "contacts-api", "http://localhost:8080")

	if rr.Code != http.StatusCreated {
		t.Fatalf("expected 201, got %d: %s", rr.Code, rr.Body.String())
	}

	var body map[string]string
	if err := json.NewDecoder(rr.Body).Decode(&body); err != nil {
		t.Fatalf("failed to decode JSON: %v", err)
	}

	if body["status"] != "registered" {
		t.Errorf("expected status=registered, got %q", body["status"])
	}
	if body["name"] != "contacts-api" {
		t.Errorf("expected name=contacts-api, got %q", body["name"])
	}
}

// ---------------------------------------------------------------------------
// TestHandleRegisterMissingFields
// ---------------------------------------------------------------------------

func TestHandleRegisterMissingFields(t *testing.T) {
	_, mux := newTestGateway()

	cases := []struct {
		label string
		body  string
	}{
		{"missing name", `{"url":"http://localhost:8080"}`},
		{"missing url", `{"name":"svc"}`},
		{"both missing", `{}`},
	}

	for _, tc := range cases {
		t.Run(tc.label, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodPost, "/services", strings.NewReader(tc.body))
			req.Header.Set("Content-Type", "application/json")
			rr := httptest.NewRecorder()
			mux.ServeHTTP(rr, req)

			if rr.Code != http.StatusBadRequest {
				t.Errorf("expected 400, got %d", rr.Code)
			}
		})
	}
}

// ---------------------------------------------------------------------------
// TestHandleListServices
// ---------------------------------------------------------------------------

func TestHandleListServices(t *testing.T) {
	_, mux := newTestGateway()

	// Register two services first.
	rr1 := registerService(t, mux, "svc-a", "http://localhost:8001")
	if rr1.Code != http.StatusCreated {
		t.Fatalf("failed to register svc-a: %d", rr1.Code)
	}
	rr2 := registerService(t, mux, "svc-b", "http://localhost:8002")
	if rr2.Code != http.StatusCreated {
		t.Fatalf("failed to register svc-b: %d", rr2.Code)
	}

	req := httptest.NewRequest(http.MethodGet, "/services", nil)
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rr.Code)
	}

	var list []map[string]any
	if err := json.NewDecoder(rr.Body).Decode(&list); err != nil {
		t.Fatalf("failed to decode JSON: %v", err)
	}

	if len(list) != 2 {
		t.Errorf("expected 2 services, got %d", len(list))
	}

	names := make(map[string]bool)
	for _, item := range list {
		names[item["name"].(string)] = true
	}
	if !names["svc-a"] || !names["svc-b"] {
		t.Errorf("expected svc-a and svc-b in list, got %v", names)
	}
}

// ---------------------------------------------------------------------------
// TestHandleDeregister
// ---------------------------------------------------------------------------

func TestHandleDeregister(t *testing.T) {
	_, mux := newTestGateway()

	rr := registerService(t, mux, "my-svc", "http://localhost:9090")
	if rr.Code != http.StatusCreated {
		t.Fatalf("failed to register: %d", rr.Code)
	}

	req := httptest.NewRequest(http.MethodDelete, "/services/my-svc", nil)
	rr2 := httptest.NewRecorder()
	mux.ServeHTTP(rr2, req)

	if rr2.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rr2.Code, rr2.Body.String())
	}

	var body map[string]string
	if err := json.NewDecoder(rr2.Body).Decode(&body); err != nil {
		t.Fatalf("failed to decode JSON: %v", err)
	}
	if body["status"] != "deregistered" {
		t.Errorf("expected status=deregistered, got %q", body["status"])
	}
}

// ---------------------------------------------------------------------------
// TestHandleDeregisterNotFound
// ---------------------------------------------------------------------------

func TestHandleDeregisterNotFound(t *testing.T) {
	_, mux := newTestGateway()

	req := httptest.NewRequest(http.MethodDelete, "/services/neexistuje", nil)
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req)

	if rr.Code != http.StatusNotFound {
		t.Errorf("expected 404, got %d", rr.Code)
	}
}

// ---------------------------------------------------------------------------
// TestHandleStats
// ---------------------------------------------------------------------------

func TestHandleStats(t *testing.T) {
	_, mux := newTestGateway()

	// Register a service so stats has at least one entry.
	registerService(t, mux, "stats-svc", "http://localhost:7070")

	req := httptest.NewRequest(http.MethodGet, "/stats", nil)
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rr.Code)
	}

	var stats map[string]map[string]int64
	if err := json.NewDecoder(rr.Body).Decode(&stats); err != nil {
		t.Fatalf("failed to decode JSON: %v", err)
	}

	entry, ok := stats["stats-svc"]
	if !ok {
		t.Fatal("expected stats-svc in stats response")
	}
	if _, ok := entry["request_count"]; !ok {
		t.Error("expected request_count field in stats entry")
	}
	if _, ok := entry["error_count"]; !ok {
		t.Error("expected error_count field in stats entry")
	}
}

// ---------------------------------------------------------------------------
// TestHandleTopology
// ---------------------------------------------------------------------------

func TestHandleTopology(t *testing.T) {
	_, mux := newTestGateway()

	// Registering a service adds a gateway-go→service edge automatically.
	registerService(t, mux, "topo-svc", "http://localhost:6060")

	req := httptest.NewRequest(http.MethodGet, "/topology", nil)
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rr.Code)
	}

	var body struct {
		Edges []TopologyEdge `json:"edges"`
	}
	if err := json.NewDecoder(rr.Body).Decode(&body); err != nil {
		t.Fatalf("failed to decode JSON: %v", err)
	}

	if len(body.Edges) == 0 {
		t.Fatal("expected at least one edge in topology")
	}

	found := false
	for _, e := range body.Edges {
		if e.From == "gateway-go" && e.To == "topo-svc" {
			found = true
			break
		}
	}
	if !found {
		t.Errorf("expected edge gateway-go→topo-svc, got %+v", body.Edges)
	}
}

// ---------------------------------------------------------------------------
// TestHandleAddEdge
// ---------------------------------------------------------------------------

func TestHandleAddEdge(t *testing.T) {
	_, mux := newTestGateway()

	body := strings.NewReader(`{"from":"service-a","to":"service-b"}`)
	req := httptest.NewRequest(http.MethodPost, "/topology/edge", body)
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req)

	if rr.Code != http.StatusCreated {
		t.Fatalf("expected 201, got %d: %s", rr.Code, rr.Body.String())
	}

	// Verify the edge appears in GET /topology.
	req2 := httptest.NewRequest(http.MethodGet, "/topology", nil)
	rr2 := httptest.NewRecorder()
	mux.ServeHTTP(rr2, req2)

	var topo struct {
		Edges []TopologyEdge `json:"edges"`
	}
	if err := json.NewDecoder(rr2.Body).Decode(&topo); err != nil {
		t.Fatalf("failed to decode topology JSON: %v", err)
	}

	found := false
	for _, e := range topo.Edges {
		if e.From == "service-a" && e.To == "service-b" {
			found = true
			break
		}
	}
	if !found {
		t.Errorf("added edge not found in topology, got %+v", topo.Edges)
	}
}

// ---------------------------------------------------------------------------
// TestHandleProxy
// ---------------------------------------------------------------------------

func TestHandleProxy(t *testing.T) {
	// Spin up a fake backend that returns 200 with a known body.
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"pong":true}`))
	}))
	defer backend.Close()

	_, mux := newTestGateway()

	// Register the fake backend under the name "backend-svc".
	rr := registerService(t, mux, "backend-svc", backend.URL)
	if rr.Code != http.StatusCreated {
		t.Fatalf("failed to register backend-svc: %d %s", rr.Code, rr.Body.String())
	}

	// Proxy a request through the gateway.
	req := httptest.NewRequest(http.MethodGet, "/backend-svc/ping", nil)
	rr2 := httptest.NewRecorder()
	mux.ServeHTTP(rr2, req)

	if rr2.Code != http.StatusOK {
		t.Fatalf("expected 200 from proxy, got %d: %s", rr2.Code, rr2.Body.String())
	}

	var body map[string]any
	if err := json.NewDecoder(rr2.Body).Decode(&body); err != nil {
		t.Fatalf("failed to decode proxied response: %v", err)
	}
	if body["pong"] != true {
		t.Errorf("unexpected proxied body: %v", body)
	}

	// Verify the request counter was incremented.
	gw := newGateway() // use a fresh one to get the registered entry reference
	_ = gw             // The counter check is done via the mux's gateway instance.

	// Check via /stats that request_count == 1 for backend-svc.
	statsReq := httptest.NewRequest(http.MethodGet, "/stats", nil)
	statsRR := httptest.NewRecorder()
	mux.ServeHTTP(statsRR, statsReq)

	var stats map[string]map[string]int64
	if err := json.NewDecoder(statsRR.Body).Decode(&stats); err != nil {
		t.Fatalf("failed to decode stats: %v", err)
	}
	if stats["backend-svc"]["request_count"] != 1 {
		t.Errorf("expected request_count=1, got %d", stats["backend-svc"]["request_count"])
	}
}
