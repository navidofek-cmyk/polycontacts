// Asynchronní HTTP server pro fulltextové vyhledávání v kontaktech.
// Používá axum (webový framework nad tokio) a drží celý index v paměti —
// žádná databáze, žádný disk, latence vyhledávání je sub-milisekundová.
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
    // AtomicU64 umožňuje čítat a přičítat čítače z více vláken současně
    // bez zámku — pro statistiky (requests, errors) to stačí a je to rychlejší.
    atomic::{AtomicU64, Ordering},
    // Arc = "Atomically Reference Counted": chceme sdílet AppState mezi
    // vlákny (handlery), aniž bychom kopírovali celá data. Arc zajistí,
    // že data žijí tak dlouho, jak je někdo potřebuje.
    Arc,
};
use std::time::Instant;
// tokio::sync::RwLock místo std::sync::RwLock — asynchronní zámek, který
// neudrží vlákno zablokované (jen pozastaví daný async task), takže server
// zvládá obsluhovat další požadavky i při čekání na zápis do indexu.
use tokio::sync::RwLock;
use tracing::{error, info, warn};

// ---------------------------------------------------------------------------
// Domain types
// ---------------------------------------------------------------------------

/// Jedno telefonní číslo s lidsky čitelným štítkem (např. "mobil", "práce").
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PhoneNumber {
    pub label: String,
    pub number: String,
}

/// Jeden kontakt — základní datová jednotka celé služby.
/// `#[serde(default)]` zajistí, že pokud JSON neobsahuje `phones` nebo
/// `category`, dostaneme prázdný vektor / prázdný řetězec místo chyby.
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

// Váhy určují, kolik bodů přidá shoda v daném poli.
// Příjmení dostane 3× více než e-mail, protože uživatelé hledají kontakty
// primárně podle jména, ne podle e-mailové adresy.
const WEIGHT_LAST_NAME: f32 = 3.0;
const WEIGHT_FIRST_NAME: f32 = 2.0;
const WEIGHT_EMAIL: f32 = 1.0;
const WEIGHT_PHONE: f32 = 1.0;

/// Invertovaný index pro fulltextové vyhledávání.
///
/// Datová struktura:
/// ```
/// inverted_index: {
///   "novak"   -> [("id-42", 3.0), ("id-7", 1.0)],
///   "jana"    -> [("id-42", 2.0)],
///   ...
/// }
/// contacts: {
///   "id-42" -> Contact { first_name: "Jana", last_name: "Novák", ... },
///   ...
/// }
/// ```
/// Při vyhledávání stačí najít token v HashMap (O(1)) a sečíst váhy.
/// Celý index žije v paměti — pro tisíce kontaktů to jsou jednotky MB.
#[derive(Debug, Default)]
pub struct SearchIndex {
    /// Všechny kontakty uložené podle jejich ID pro O(1) přístup při skládání výsledků.
    pub contacts: HashMap<String, Contact>,
    /// Invertovaný index: token (slovo) → seznam (contact_id, váha).
    /// Váhy jsou kumulativní — jedno slovo může patřit do více polí jednoho kontaktu
    /// (např. příjmení = součást e-mailu), váhy se sečtou.
    pub inverted_index: HashMap<String, Vec<(String, f32)>>,
}

/// Rozdělí vstupní řetězec na tokeny vhodné pro indexování i vyhledávání.
///
/// Proč lowercase + split na ne-alfanumerické znaky:
/// - Uživatel píše "Jana" i "jana" — chceme shodu v obou případech.
/// - Oddělujeme na tečkách, pomlčkách atd., aby "jana.novakova@..." dalo
///   tokeny ["jana", "novakova", "example", "com"].
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

    /// Přebuduje invertovaný index ze současného obsahu `self.contacts`.
    ///
    /// Volá se po každém POST /index (tj. když přijde nová sada kontaktů).
    /// Celý index se vždy vymaže a postaví znovu — nevznikají "mrtvé" záznamy
    /// po smazaných kontaktech.
    pub fn rebuild(&mut self) {
        self.inverted_index.clear();

        for contact in self.contacts.values() {
            let id = contact.id.clone();

            // Closure zachycená mutabilně (`mut`) — přidá všechny tokeny
            // z `text` do indexu s danou váhou pro aktuální kontakt.
            // Closure je definovaná uvnitř smyčky, aby měla přístup k `id`
            // bez nutnosti předávat ho jako parametr.
            let mut add_tokens = |text: &str, weight: f32| {
                for token in tokenize(text) {
                    let entry = self
                        .inverted_index
                        .entry(token)
                        .or_default();

                    // Pokud tento kontakt pro daný token již existuje
                    // (např. slovo je v příjmení i v e-mailu), váhy se sečtou.
                    // Tím jeden kontakt nezabírá zbytečně více záznamů v posting-listu.
                    if let Some(pair) = entry.iter_mut().find(|(cid, _)| cid == &id) {
                        pair.1 += weight;
                    } else {
                        entry.push((id.clone(), weight));
                    }
                }
            };

            // Pole indexujeme sestupně podle váhy — pořadí zde nehraje roli,
            // ale pomáhá orientaci: nejdůležitější pole na prvním místě.
            add_tokens(&contact.last_name, WEIGHT_LAST_NAME);
            add_tokens(&contact.first_name, WEIGHT_FIRST_NAME);
            add_tokens(&contact.email, WEIGHT_EMAIL);
            for phone in &contact.phones {
                add_tokens(&phone.number, WEIGHT_PHONE);
                add_tokens(&phone.label, WEIGHT_PHONE);
            }
            // Kategorie dostane stejnou váhu jako e-mail — je doplňková informace.
            add_tokens(&contact.category, WEIGHT_EMAIL);
        }
    }

    /// Prohledá index a vrátí kontakty seřazené sestupně podle skóre.
    ///
    /// Algoritmus kombinuje dva typy shody:
    ///
    /// 1. **Exact match** — dotazovaný token se přesně shoduje s indexovaným tokenem.
    ///    Skóre = plná váha z indexu.
    ///
    /// 2. **Prefix match** — dotazovaný token je předponou indexovaného tokenu
    ///    (např. "Nov" najde "Nováková"). Skóre = váha × 0.5 (penalizace za neúplnost).
    ///    Díky tomu funguje vyhledávání během psaní (incremental search).
    ///
    /// Výsledné skóre kontaktu = součet skóre přes všechny tokeny dotazu.
    pub fn search(&self, query: &str) -> Vec<(Contact, f32)> {
        let tokens = tokenize(query);
        if tokens.is_empty() {
            return Vec::new();
        }

        // scores: contact_id → celkové skóre pro tento dotaz.
        // Používáme &str místo String, abychom nealokovali nové řetězce —
        // klíče ukazují přímo do `self.contacts`.
        let mut scores: HashMap<&str, f32> = HashMap::new();

        for token in &tokens {
            // --- Přesná shoda (exact match) ---
            if let Some(postings) = self.inverted_index.get(token.as_str()) {
                for (cid, weight) in postings {
                    *scores.entry(cid.as_str()).or_default() += weight;
                }
            }

            // --- Prefixová shoda (prefix match) ---
            // Procházíme celý index a hledáme tokeny, jejichž začátek odpovídá
            // dotazu. Je to O(n) operace přes počet unikátních tokenů v indexu,
            // ale pro tisíce kontaktů je to stále velmi rychlé (< 1 ms).
            for (indexed_token, postings) in &self.inverted_index {
                if indexed_token != token && indexed_token.starts_with(token.as_str()) {
                    // Penalizace 0.5: prefix-shoda je méně jistá než přesná shoda.
                    // Hodnota 0.5 je kompromis — dost nízká, aby exact-match vždy
                    // předčil prefix-match při stejném základním skóre, ale dost
                    // vysoká, aby prefix-výsledky nezmizely úplně ze seznamu.
                    let prefix_weight = 0.5;
                    for (cid, weight) in postings {
                        *scores.entry(cid.as_str()).or_default() += weight * prefix_weight;
                    }
                }
            }
        }

        let mut results: Vec<(Contact, f32)> = scores
            .into_iter()
            .filter_map(|(cid, score)| {
                // Přeložíme contact_id zpět na plný Contact objekt.
                self.contacts.get(cid).map(|c| (c.clone(), score))
            })
            .collect();

        // Řadíme sestupně (nejvyšší skóre první). `partial_cmp` je potřeba proto,
        // že f32 může být NaN — v praxi se to nestane, ale kompilátor to vyžaduje.
        results.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
        results
    }
}

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------

/// Sdílený stav celé aplikace — jedna instance, sdílená mezi všemi handlery.
///
/// `Clone` je levná operace: klonuje se jen Arc (čítač referencí se zvýší o 1),
/// nikoli samotná data uvnitř.
#[derive(Clone)]
pub struct AppState {
    /// Invertovaný index chráněný čtecím/zápisovým zámkem.
    /// Arc<RwLock<...>>: Arc proto, že stav sdílí více vláken;
    /// RwLock proto, že čtení (vyhledávání) může probíhat paralelně,
    /// zápis (indexování) má exkluzivní přístup.
    pub index: Arc<RwLock<SearchIndex>>,
    /// Počet všech přijatých HTTP požadavků od spuštění služby.
    pub requests: Arc<AtomicU64>,
    /// Počet chybových odpovědí (pro monitoring / alerty).
    pub errors: Arc<AtomicU64>,
    /// Čas spuštění — pro výpočet uptime v endpointu /stats.
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

/// Tělo POST /index požadavku — seznam kontaktů k zindexování.
#[derive(Deserialize)]
pub struct IndexBody {
    pub contacts: Vec<Contact>,
}

/// Query parametry GET /search — `q` je hledaný výraz.
/// `Option<String>` proto, aby `/search` bez `?q=` nevrátilo chybu 422.
#[derive(Deserialize)]
pub struct SearchParams {
    pub q: Option<String>,
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

/// POST /index  — přijme seznam kontaktů a přebuduje celý vyhledávací index.
///
/// Starý index se vždy celý nahradí novým — klient posílá kompletní snapshot,
/// ne jen diff. Tím se zabrání nekonzistencím při mazání kontaktů.
async fn handle_index(
    State(state): State<AppState>,
    Json(body): Json<IndexBody>,
) -> Result<Json<Value>, (StatusCode, Json<Value>)> {
    // Ordering::Relaxed: čítač nemusí být synchronizován s jinými operacemi,
    // stačí atomicita samotného přičtení. Pro statistiky je to dostačující.
    state.requests.fetch_add(1, Ordering::Relaxed);

    let count = body.contacts.len();
    // Získáme exkluzivní zápis — během rebuildu žádné vyhledávání neprojde.
    // `.await` pozastaví tento task (ne celé vlákno) dokud zámek není volný.
    let mut idx = state.index.write().await;
    idx.contacts.clear();
    for c in body.contacts {
        idx.contacts.insert(c.id.clone(), c);
    }
    idx.rebuild();

    info!(count, "index rebuilt");
    Ok(Json(json!({ "indexed": count })))
}

/// GET /search?q=...  — vrátí seznam kontaktů odpovídajících dotazu.
///
/// Skóre se do odpovědi nezahrnuje — klient ho nepotřebuje,
/// pořadí výsledků samo o sobě nese informaci o relevanci.
async fn handle_search(
    State(state): State<AppState>,
    Query(params): Query<SearchParams>,
) -> Result<Json<Value>, (StatusCode, Json<Value>)> {
    state.requests.fetch_add(1, Ordering::Relaxed);

    // Prázdný dotaz (chybějící ?q) zachováme jako prázdný řetězec —
    // tokenize("") vrátí prázdný vektor a search vrátí prázdný výsledek.
    let query = params.q.unwrap_or_default();
    let started = Instant::now();

    // Sdílené čtení — více vyhledávání může běžet paralelně.
    let idx = state.index.read().await;
    let results: Vec<Contact> = idx
        .search(&query)
        .into_iter()
        .map(|(c, _score)| c)   // skóre sloužilo jen pro řazení, do JSON ho nezahrnujeme
        .collect();

    let took_ms = started.elapsed().as_millis() as u64;
    let total = results.len();

    Ok(Json(json!({
        "results": results,
        "total": total,
        "took_ms": took_ms,   // pro ladění výkonu na straně klienta / monitoringu
    })))
}

/// GET /health  — jednoduchý liveness probe pro load balancer / Kubernetes.
async fn handle_health() -> Json<Value> {
    Json(json!({ "status": "ok", "service": "search-rust" }))
}

/// GET /stats  — metriky pro monitoring (Prometheus scraping nebo ruční kontrola).
async fn handle_stats(State(state): State<AppState>) -> Json<Value> {
    // Ordering::Relaxed: čteme statistiku — nevyžadujeme happens-before garanci
    // vůči jiným operacím, jen chceme aktuální hodnotu čítače.
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

/// Inicializační sekvence spouštěná asynchronně po startu serveru.
///
/// Server začne přijímat požadavky okamžitě (index je prázdný),
/// startup běží na pozadí. Tím se zkrátí čas do prvního zdravého
/// health-check response — load balancer nemusí čekat na načtení dat.
///
/// Kroky:
/// 1. Načte kontakty z C++ services (opakuje dokud neuspěje).
/// 2. POSTuje je na vlastní /index endpoint.
/// 3. Registruje se u API gateway.
async fn startup(_state: AppState, self_url: String) {
    let client = Client::new();

    let contacts_url = std::env::var("CONTACTS_URL")
        .unwrap_or_else(|_| "http://localhost:8080".to_string());
    let gateway_url = std::env::var("GATEWAY_URL")
        .unwrap_or_else(|_| "http://localhost:9000".to_string());

    // --- Krok 1: Načtení kontaktů (retry loop) ---
    // Používáme nekonečnou smyčku s `break` při úspěchu — idiomatický Rust
    // pro "opakuj dokud nevyjde". Rust neumí do-while, ale `loop { break value }`
    // je ekvivalent a navíc vrací hodnotu (zde Vec<Contact>).
    let contacts_endpoint = format!("{}/contacts", contacts_url);
    let contacts: Vec<Contact> = loop {
        info!("Fetching contacts from {}", contacts_endpoint);
        match client.get(&contacts_endpoint).send().await {
            Ok(resp) if resp.status().is_success() => {
                match resp.json::<Vec<Contact>>().await {
                    Ok(list) => {
                        info!(count = list.len(), "fetched contacts");
                        break list;  // úspěch — opustíme loop a vrátíme list
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
        // Před dalším pokusem počkáme — jinak bychom zahlcili síť/logy.
        tokio::time::sleep(tokio::time::Duration::from_secs(2)).await;
    };

    // --- Krok 2: Zaindexování kontaktů přes vlastní HTTP API ---
    // Jde o záměrně nepřímou cestu (self-call) — tím se otestuje celý stack
    // a zaindexování sdílí stejnou logiku se standardním POST /index.
    let index_endpoint = format!("{}/index", self_url);
    let body = json!({ "contacts": contacts });
    match client.post(&index_endpoint).json(&body).send().await {
        Ok(resp) => info!("Self-index response: {}", resp.status()),
        Err(e) => error!("Failed to self-index: {}", e),
    }

    // --- Krok 3: Registrace u API gateway ---
    // Gateway potřebuje vědět, kde služba běží a jak ověřit její zdraví.
    // Chyba registrace je jen varování — server funguje i bez gateway.
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

/// Vstupní bod aplikace.
///
/// `#[tokio::main]` je makro, které obalí `main` do tokio asynchronního runtime —
/// bez něj by `async fn main` nebylo možné přeložit. Runtime spravuje thread pool
/// a plánování async tasků.
#[tokio::main]
async fn main() {
    // Inicializace logování do stdout ve formátu vhodném pro terminál i log agregátory.
    tracing_subscriber::fmt::init();

    let state = AppState::new();
    let self_url = std::env::var("SELF_URL")
        .unwrap_or_else(|_| "http://localhost:8081".to_string());

    // Spustíme inicializační sekvenci jako samostatný async task na pozadí.
    // `tokio::spawn` je ekvivalent spuštění vlákna, ale je to "zelené vlákno"
    // (coroutine) — velmi levné, nepotřebuje vlastní OS vlákno.
    // Blok `{}` zajistí, že state_clone a self_url_clone jsou přesunuty do
    // async bloku přes `move`, ale proměnné ze scope main zůstanou dostupné dále.
    {
        let state_clone = state.clone();  // Arc clone — levná operace
        let self_url_clone = self_url.clone();
        tokio::spawn(async move {
            startup(state_clone, self_url_clone).await;
        });
    }

    // Sestavení routeru — každý endpoint mapuje na svůj handler.
    let app = Router::new()
        .route("/index", post(handle_index))
        .route("/search", get(handle_search))
        .route("/health", get(handle_health))
        .route("/stats", get(handle_stats))
        .with_state(state);  // stav se zkopíruje (Arc clone) do každého handleru

    let listener = tokio::net::TcpListener::bind("0.0.0.0:8081")
        .await
        .expect("Failed to bind to 0.0.0.0:8081");

    info!("search-rust listening on 0.0.0.0:8081");
    // `axum::serve` blokuje (await) do té doby, než dojde k fatální chybě serveru.
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
