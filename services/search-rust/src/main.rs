use axum::{
    extract::{Query, State},
    http::StatusCode,
    response::Json,
    routing::{get, post},
    Router,
};
use reqwest::Client;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::{
    atomic::{AtomicU64, Ordering},
    Arc,
};
use std::time::Instant;
use tokio::sync::RwLock;
use tracing::{error, info, warn};

// ---------------------------------------------------------------------------
// Domain types
// ---------------------------------------------------------------------------

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PhoneNumber {
    pub label: String,
    pub number: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Contact {
    pub id: String,
    pub first_name: String,
    pub last_name: String,
    pub email: String,
    #[serde(default)]
    pub phones: Vec<PhoneNumber>,
    #[serde(default)]
    pub category: String,
}

// ---------------------------------------------------------------------------
// Search index
// ---------------------------------------------------------------------------

/// Weight assigned to each field when building the inverted index.
const WEIGHT_LAST_NAME: f32 = 3.0;
const WEIGHT_FIRST_NAME: f32 = 2.0;
const WEIGHT_EMAIL: f32 = 1.0;
const WEIGHT_PHONE: f32 = 1.0;

/// Per-contact, per-token accumulated weight stored in the index.
/// inverted_index: token -> Vec<(contact_id, weight)>
#[derive(Debug, Default)]
pub struct SearchIndex {
    pub contacts: HashMap<String, Contact>,
    /// token -> list of (contact_id, accumulated_weight) pairs
    pub inverted_index: HashMap<String, Vec<(String, f32)>>,
}

/// Tokenise a string: lowercase, split on non-alphanumeric chars, skip empty.
fn tokenize(s: &str) -> Vec<String> {
    s.to_lowercase()
        .split(|c: char| !c.is_alphanumeric())
        .filter(|t| !t.is_empty())
        .map(String::from)
        .collect()
}

impl SearchIndex {
    pub fn new() -> Self {
        Self::default()
    }

    /// Rebuild the inverted index from the current `contacts` map.
    pub fn rebuild(&mut self) {
        self.inverted_index.clear();

        for contact in self.contacts.values() {
            let id = contact.id.clone();

            // Helper: add every token from `text` with the given weight.
            let mut add_tokens = |text: &str, weight: f32| {
                for token in tokenize(text) {
                    let entry = self
                        .inverted_index
                        .entry(token)
                        .or_default();

                    // Accumulate weight for this contact within the same token.
                    if let Some(pair) = entry.iter_mut().find(|(cid, _)| cid == &id) {
                        pair.1 += weight;
                    } else {
                        entry.push((id.clone(), weight));
                    }
                }
            };

            add_tokens(&contact.last_name, WEIGHT_LAST_NAME);
            add_tokens(&contact.first_name, WEIGHT_FIRST_NAME);
            add_tokens(&contact.email, WEIGHT_EMAIL);
            for phone in &contact.phones {
                add_tokens(&phone.number, WEIGHT_PHONE);
                add_tokens(&phone.label, WEIGHT_PHONE);
            }
            add_tokens(&contact.category, WEIGHT_EMAIL);
        }
    }

    /// Search the index for `query`.  Returns contacts sorted by descending score.
    pub fn search(&self, query: &str) -> Vec<(Contact, f32)> {
        let tokens = tokenize(query);
        if tokens.is_empty() {
            return Vec::new();
        }

        let mut scores: HashMap<&str, f32> = HashMap::new();

        for token in &tokens {
            // Exact match
            if let Some(postings) = self.inverted_index.get(token.as_str()) {
                for (cid, weight) in postings {
                    *scores.entry(cid.as_str()).or_default() += weight;
                }
            }
            // Prefix match (token is a prefix of an indexed token)
            for (indexed_token, postings) in &self.inverted_index {
                if indexed_token != token && indexed_token.starts_with(token.as_str()) {
                    let prefix_weight = 0.5; // partial-match penalty
                    for (cid, weight) in postings {
                        *scores.entry(cid.as_str()).or_default() += weight * prefix_weight;
                    }
                }
            }
        }

        let mut results: Vec<(Contact, f32)> = scores
            .into_iter()
            .filter_map(|(cid, score)| {
                self.contacts.get(cid).map(|c| (c.clone(), score))
            })
            .collect();

        results.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
        results
    }
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

#[derive(Clone)]
pub struct AppState {
    pub index: Arc<RwLock<SearchIndex>>,
    pub requests: Arc<AtomicU64>,
    pub errors: Arc<AtomicU64>,
    pub start_time: Instant,
}

impl AppState {
    pub fn new() -> Self {
        Self {
            index: Arc::new(RwLock::new(SearchIndex::new())),
            requests: Arc::new(AtomicU64::new(0)),
            errors: Arc::new(AtomicU64::new(0)),
            start_time: Instant::now(),
        }
    }
}

// ---------------------------------------------------------------------------
// Request / response helpers
// ---------------------------------------------------------------------------

#[derive(Deserialize)]
pub struct IndexBody {
    pub contacts: Vec<Contact>,
}

#[derive(Deserialize)]
pub struct SearchParams {
    pub q: Option<String>,
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

/// POST /index  — body: {"contacts":[...]}
async fn handle_index(
    State(state): State<AppState>,
    Json(body): Json<IndexBody>,
) -> Result<Json<Value>, (StatusCode, Json<Value>)> {
    state.requests.fetch_add(1, Ordering::Relaxed);

    let count = body.contacts.len();
    let mut idx = state.index.write().await;
    idx.contacts.clear();
    for c in body.contacts {
        idx.contacts.insert(c.id.clone(), c);
    }
    idx.rebuild();

    info!(count, "index rebuilt");
    Ok(Json(json!({ "indexed": count })))
}

/// GET /search?q=...
async fn handle_search(
    State(state): State<AppState>,
    Query(params): Query<SearchParams>,
) -> Result<Json<Value>, (StatusCode, Json<Value>)> {
    state.requests.fetch_add(1, Ordering::Relaxed);

    let query = params.q.unwrap_or_default();
    let started = Instant::now();

    let idx = state.index.read().await;
    let results: Vec<Contact> = idx
        .search(&query)
        .into_iter()
        .map(|(c, _score)| c)
        .collect();

    let took_ms = started.elapsed().as_millis() as u64;
    let total = results.len();

    Ok(Json(json!({
        "results": results,
        "total": total,
        "took_ms": took_ms,
    })))
}

/// GET /health
async fn handle_health() -> Json<Value> {
    Json(json!({ "status": "ok", "service": "search-rust" }))
}

/// GET /stats
async fn handle_stats(State(state): State<AppState>) -> Json<Value> {
    let requests = state.requests.load(Ordering::Relaxed);
    let errors = state.errors.load(Ordering::Relaxed);
    let uptime_s = state.start_time.elapsed().as_secs();

    Json(json!({
        "requests": requests,
        "errors": errors,
        "uptime_s": uptime_s,
    }))
}

// ---------------------------------------------------------------------------
// Startup: fetch contacts from C++ service, index them, register with gateway
// ---------------------------------------------------------------------------

async fn startup(_state: AppState, self_url: String) {
    let client = Client::new();

    let contacts_url = std::env::var("CONTACTS_URL")
        .unwrap_or_else(|_| "http://localhost:8080".to_string());
    let gateway_url = std::env::var("GATEWAY_URL")
        .unwrap_or_else(|_| "http://localhost:9000".to_string());

    // 1. Fetch contacts (retry until success)
    let contacts_endpoint = format!("{}/contacts", contacts_url);
    let contacts: Vec<Contact> = loop {
        info!("Fetching contacts from {}", contacts_endpoint);
        match client.get(&contacts_endpoint).send().await {
            Ok(resp) if resp.status().is_success() => {
                match resp.json::<Vec<Contact>>().await {
                    Ok(list) => {
                        info!(count = list.len(), "fetched contacts");
                        break list;
                    }
                    Err(e) => {
                        warn!("Failed to parse contacts response: {}", e);
                    }
                }
            }
            Ok(resp) => {
                warn!("Contacts service returned {}", resp.status());
            }
            Err(e) => {
                warn!("Could not reach contacts service: {}", e);
            }
        }
        tokio::time::sleep(tokio::time::Duration::from_secs(2)).await;
    };

    // 2. POST to self /index
    let index_endpoint = format!("{}/index", self_url);
    let body = json!({ "contacts": contacts });
    match client.post(&index_endpoint).json(&body).send().await {
        Ok(resp) => info!("Self-index response: {}", resp.status()),
        Err(e) => error!("Failed to self-index: {}", e),
    }

    // 3. Register with gateway
    let register_endpoint = format!("{}/services", gateway_url);
    let reg_body = json!({
        "name": "search-rust",
        "url": self_url,
        "health": format!("{}/health", self_url),
    });
    match client.post(&register_endpoint).json(&reg_body).send().await {
        Ok(resp) => info!("Gateway registration: {}", resp.status()),
        Err(e) => warn!("Could not register with gateway: {}", e),
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();

    let state = AppState::new();
    let self_url = std::env::var("SELF_URL")
        .unwrap_or_else(|_| "http://localhost:8081".to_string());

    // Kick off startup tasks in the background.
    {
        let state_clone = state.clone();
        let self_url_clone = self_url.clone();
        tokio::spawn(async move {
            startup(state_clone, self_url_clone).await;
        });
    }

    let app = Router::new()
        .route("/index", post(handle_index))
        .route("/search", get(handle_search))
        .route("/health", get(handle_health))
        .route("/stats", get(handle_stats))
        .with_state(state);

    let listener = tokio::net::TcpListener::bind("0.0.0.0:8081")
        .await
        .expect("Failed to bind to 0.0.0.0:8081");

    info!("search-rust listening on 0.0.0.0:8081");
    axum::serve(listener, app)
        .await
        .expect("Server error");
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use axum::{
        body::Body,
        http::{Request, StatusCode},
    };
    use http_body_util::BodyExt;
    use tower::ServiceExt; // for `oneshot`

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    fn make_contact(id: &str, first: &str, last: &str, email: &str) -> Contact {
        Contact {
            id: id.to_string(),
            first_name: first.to_string(),
            last_name: last.to_string(),
            email: email.to_string(),
            phones: vec![],
            category: String::new(),
        }
    }

    fn build_app() -> Router {
        let state = AppState::new();
        Router::new()
            .route("/index", axum::routing::post(handle_index))
            .route("/search", axum::routing::get(handle_search))
            .route("/health", axum::routing::get(handle_health))
            .route("/stats", axum::routing::get(handle_stats))
            .with_state(state)
    }

    async fn body_to_value(body: axum::body::Body) -> serde_json::Value {
        let bytes = body.collect().await.unwrap().to_bytes();
        serde_json::from_slice(&bytes).unwrap()
    }

    // -----------------------------------------------------------------------
    // SearchIndex unit tests
    // -----------------------------------------------------------------------

    #[test]
    fn test_tokenize() {
        let tokens = tokenize("Jana Nováková");
        assert_eq!(tokens, vec!["jana", "nováková"]);
    }

    #[test]
    fn test_empty_search() {
        let index = SearchIndex::new();
        let results = index.search("");
        assert!(results.is_empty());
    }

    #[test]
    fn test_exact_match_last_name_higher_than_email() {
        let mut index = SearchIndex::new();
        // Contact whose last_name token matches and email token also matches the same word.
        let c = Contact {
            id: "1".to_string(),
            first_name: "Test".to_string(),
            last_name: "novakova".to_string(),
            email: "novakova@example.com".to_string(),
            phones: vec![],
            category: String::new(),
        };
        index.contacts.insert(c.id.clone(), c);
        index.rebuild();

        // "novakova" hits last_name (weight 3.0) AND email (weight 1.0) → total 4.0
        let results = index.search("novakova");
        assert_eq!(results.len(), 1);
        // Score must come from last_name (3.0) + email (1.0) = 4.0
        let score = results[0].1;
        assert!(score >= WEIGHT_LAST_NAME, "score {score} should be >= WEIGHT_LAST_NAME");

        // Compare two contacts: one matched via last_name, one via email only.
        let mut index2 = SearchIndex::new();
        let by_last = make_contact("a", "Alice", "Jana", "alice@example.com");
        let by_email = make_contact("b", "Bob", "Smith", "jana@example.com");
        index2.contacts.insert("a".to_string(), by_last);
        index2.contacts.insert("b".to_string(), by_email);
        index2.rebuild();

        let results2 = index2.search("jana");
        assert_eq!(results2.len(), 2);
        // First result should be the one matched via last_name.
        assert_eq!(results2[0].0.id, "a");
        assert!(results2[0].1 > results2[1].1);
    }

    #[test]
    fn test_prefix_match() {
        let mut index = SearchIndex::new();
        let c = make_contact("1", "Jana", "Nováková", "jana@example.com");
        index.contacts.insert(c.id.clone(), c);
        index.rebuild();

        let results = index.search("Nov");
        assert_eq!(results.len(), 1, "prefix 'Nov' should find Nováková");
        assert_eq!(results[0].0.id, "1");
    }

    #[test]
    fn test_weight_last_name_higher_than_first_name() {
        let mut index = SearchIndex::new();
        // "test" appears as first_name for contact A and as last_name for contact B.
        let a = make_contact("a", "Test", "Other", "a@x.com");
        let b = make_contact("b", "Other", "Test", "b@x.com");
        index.contacts.insert("a".to_string(), a);
        index.contacts.insert("b".to_string(), b);
        index.rebuild();

        let results = index.search("test");
        assert_eq!(results.len(), 2);
        // Contact b matched via last_name (weight 3.0) should rank higher than
        // contact a matched via first_name (weight 2.0).
        assert_eq!(results[0].0.id, "b", "last_name match should rank higher");
        assert!(
            results[0].1 > results[1].1,
            "last_name score ({}) should exceed first_name score ({})",
            results[0].1,
            results[1].1
        );
    }

    #[test]
    fn test_rebuild_clears_index() {
        let mut index = SearchIndex::new();
        let c = make_contact("1", "Jana", "Nováková", "jana@example.com");
        index.contacts.insert(c.id.clone(), c);
        index.rebuild();
        assert!(!index.inverted_index.is_empty(), "index should be populated after first rebuild");

        // Remove all contacts and rebuild — index must be cleared.
        index.contacts.clear();
        index.rebuild();
        assert!(
            index.inverted_index.is_empty(),
            "inverted_index should be empty after rebuild with no contacts"
        );
    }

    // -----------------------------------------------------------------------
    // Axum handler tests
    // -----------------------------------------------------------------------

    #[tokio::test]
    async fn test_health_endpoint() {
        let app = build_app();
        let request = Request::builder()
            .uri("/health")
            .body(Body::empty())
            .unwrap();

        let response = app.oneshot(request).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);

        let json = body_to_value(response.into_body()).await;
        assert_eq!(json["status"], "ok");
    }

    #[tokio::test]
    async fn test_search_empty_query() {
        let app = build_app();
        let request = Request::builder()
            .uri("/search")
            .body(Body::empty())
            .unwrap();

        let response = app.oneshot(request).await.unwrap();
        assert_eq!(response.status(), StatusCode::OK);

        let json = body_to_value(response.into_body()).await;
        let results = json["results"].as_array().unwrap();
        assert!(results.is_empty(), "empty query should return no results");
    }

    #[tokio::test]
    async fn test_index_and_search() {
        // First request: POST /index with a contact.
        let contacts_json = serde_json::json!({
            "contacts": [
                {
                    "id": "1",
                    "first_name": "Jana",
                    "last_name": "Nováková",
                    "email": "jana.novakova@example.com",
                    "phones": [],
                    "category": ""
                }
            ]
        });

        let index_req = Request::builder()
            .method("POST")
            .uri("/index")
            .header("content-type", "application/json")
            .body(Body::from(contacts_json.to_string()))
            .unwrap();

        // We need to send two requests through the same app state, so we can't
        // consume the app with oneshot twice directly. Build the full service
        // manually instead.
        let state = AppState::new();
        let app = Router::new()
            .route("/index", axum::routing::post(handle_index))
            .route("/search", axum::routing::get(handle_search))
            .route("/health", axum::routing::get(handle_health))
            .route("/stats", axum::routing::get(handle_stats))
            .with_state(state);

        // Clone the service so we can call it twice.
        let index_resp = app.clone().oneshot(index_req).await.unwrap();
        assert_eq!(index_resp.status(), StatusCode::OK);

        let index_json = body_to_value(index_resp.into_body()).await;
        assert_eq!(index_json["indexed"], 1);

        // Second request: GET /search?q=jana
        let search_req = Request::builder()
            .uri("/search?q=jana")
            .body(Body::empty())
            .unwrap();

        let search_resp = app.oneshot(search_req).await.unwrap();
        assert_eq!(search_resp.status(), StatusCode::OK);

        let search_json = body_to_value(search_resp.into_body()).await;
        let results = search_json["results"].as_array().unwrap();
        assert_eq!(results.len(), 1, "should find Jana Nováková");
        assert_eq!(results[0]["first_name"], "Jana");
        assert_eq!(results[0]["last_name"], "Nováková");
    }
}
