# search-rust — Průvodce kódem

## Teorie: Invertovaný index

Přímé vyhledávání (`SELECT * FROM contacts WHERE name LIKE '%jan%'`) projde každý řádek — O(n) při každém dotazu. Při 10 000 kontaktech a 100 req/s to zvládneme, ale při 1 000 000 kontaktech nebo složitějších dotazech to přestane stačit.

**Invertovaný index** obrátí vztah: místo "kontakt → slova" ukládáme "slovo → seznam kontaktů":

```
Přímý přístup:
  kontakt_1 = {first: "Jana", last: "Nováková", email: "jana@..."}
  kontakt_2 = {first: "Jan",  last: "Novák",    email: "jan@..."}

Invertovaný index:
  "jana"    → [kontakt_1]
  "novakova"→ [kontakt_1]
  "jan"     → [kontakt_1, kontakt_2]   ← prefix "jan" matchuje oba
  "novak"   → [kontakt_2]
```

Vyhledávání dotazu "jan novak" pak:
1. Tokenizuje dotaz → ["jan", "novak"]
2. Pro každý token najde odpovídající seznam v indexu (O(1) hash lookup)
3. Agreguje skóre podle váh (příjmení = 3×, jméno = 2×, email = 1×)
4. Seřadí výsledky sestupně dle skóre

Výsledek: latence nezávisí na počtu kontaktů, ale na délce dotazu. Stejný princip používá Elasticsearch, Lucene nebo PostgreSQL `tsvector`.

## Teorie: Ownership a Borrow Checker

Rust garantuje memory safety **bez garbage collectoru** pomocí systému vlastnictví (ownership):

1. **Každá hodnota má právě jednoho vlastníka.** Když vlastník zaniká, hodnota se uvolní.
2. **Půjčování (borrowing).** Hodnotu lze půjčit jako `&T` (sdílená reference, pouze čtení) nebo `&mut T` (exkluzivní reference, čtení i zápis). Obojí zároveň není možné.
3. **Žádné dangling references.** Kompilátor ověří, že reference nepřežije hodnotu na kterou ukazuje.

```rust
let index = Arc::new(RwLock::new(SearchIndex::new()));

// Sdílíme index mezi vlákny přes Arc (Atomic Reference Counting)
// Arc zajistí, že index žije dokud existuje aspoň jedna kopie Arc

let idx = index.read().await;   // sdílená reference — může číst více vláken najednou
// idx.write() — exkluzivní — blokuje dokud všichni čtenáři neskončí
```

Kompilátor odmítne kód, který by způsobil data race nebo use-after-free. Chyby které v C++ najdeme za běhu (nebo vůbec nenajdeme), Rust odmítne zkompilovat.

## Teorie: Async/await a Tokio runtime

`async fn` v Rustu je syntaktický cukr — kompilátor z ní vygeneruje stavový automat (state machine). Každý `await` bod je místo kde může být vykonávání pozastaveno a obnoveno.

Tokio runtime spravuje **executor** — thread pool který vybírá připravené tasky a spouští je. Jedno OS vlákno může obsloužit tisíce async tasků, protože při čekání na I/O (síť, disk) task uvolní vlákno a executor ho může použít pro jiný task.

```
OS vlákno 1:  task_A (čte ze sítě →) [čeká] task_B (odpovídá) task_C (čte ze sítě →) [čeká]
OS vlákno 2:  task_D (zpracovává) task_E (odpovídá) task_A (pokračuje po příchodu dat)
```

Oproti thread-per-request modelu (Apache): 1 000 souběžných spojení = 1 000 OS vláken × ~1 MB = 1 GB jen pro zásobníky. S async: stovky tasků na jedno vlákno, každý task zabírá jen nutný stav (kilobajty).

Tento dokument prochází celý zdrojový kód služby `search-rust` krok po kroku. Místo abstraktního přehledu rovnou vysvětlujeme každý řádek — proč tam je a co by se stalo, kdyby tam nebyl.

## Přehled

`search-rust` je asynchronní HTTP server pro fulltextové vyhledávání v kontaktech. Drží celý vyhledávací index v paměti RAM — žádná databáze, žádný disk. Latence vyhledávání je sub-milisekundová.

**Proč Rust?**

- **Žádný garbage collector.** Latence je konzistentní — p50 i p99.9 se liší jen 2–3×. V JVM nebo Python procesech mohou GC pauzy způsobit výkyvy o řád výše.
- **`tokio::sync::RwLock`.** Asynchronní čtecí/zápisový zámek umožňuje stovkám souběžných vyhledávání běžet paralelně, aniž by blokovala OS vlákna.
- **`async`/`await` bez blokování.** `tokio::spawn` spustí inicializační sekvenci jako lehký task. Server začne přijímat požadavky okamžitě, index se plní na pozadí.

Celá služba poskytuje čtyři HTTP endpointy:

| Metoda | Cesta | Popis |
|--------|-------|-------|
| `POST` | `/index` | Přijme seznam kontaktů a přebuduje index |
| `GET` | `/search?q=...` | Vrátí kontakty odpovídající dotazu |
| `GET` | `/health` | Liveness probe pro load balancer |
| `GET` | `/stats` | Počitadla požadavků a uptime |

---

## Cargo.toml závislosti

```toml
[dependencies]
axum = "0.7"
tokio = { version = "1", features = ["full"] }
serde = { version = "1", features = ["derive"] }
serde_json = "1"
reqwest = { version = "0.12", features = ["json"] }
tracing = "0.1"
tracing-subscriber = "0.3"

[dev-dependencies]
tower = { version = "0.5", features = ["util"] }
http-body-util = "0.1"
```

**`axum`** — webový framework postavený nad `tokio` a `hyper`. Zajišťuje routing, extrakci parametrů a automatickou (de)serializaci JSON. Alternativou by byl `actix-web` nebo `warp`, ale `axum` má nejpřímější integraci s `tokio` ekosystémem.

**`tokio`** s `features = ["full"]` — asynchronní runtime. Bez něj by `async fn` byly jen definice bez spouštěče. Feature `full` zapne všechny součásti: časovače, síťový stack, synchronizační primitiva. V produkci lze zapnout jen to, co skutečně používáme (`rt-multi-thread`, `net`, `time`), ale pro jednoduchost je `full` v pořádku.

**`serde`** s `features = ["derive"]` — framework pro serializaci a deserializaci. Feature `derive` umožňuje automaticky generovat implementaci pomocí `#[derive(Serialize, Deserialize)]` bez ručního psaní kódu.

**`serde_json`** — implementace JSON pro `serde`. Poskytuje makro `json!` pro snadné sestavení JSON hodnot a typ `Value` pro dynamická JSON data.

**`reqwest`** s `features = ["json"]` — HTTP klient. Používá se ve funkci `startup()` pro načtení kontaktů z C++ služby a pro registraci u API gateway. Feature `json` přidá metody `.json()` pro přímou (de)serializaci.

**`tracing`** — strukturované logování. Namísto `println!` používáme makra `info!`, `warn!`, `error!`, která umožňují přidat klíč-hodnota metadata (`info!(count = list.len(), "fetched contacts")`). Výstup lze nasměrovat do různých formátů (JSON pro log agregátory, barevný text pro terminál).

**`tracing-subscriber`** — konfigurace výstupu pro `tracing`. `tracing_subscriber::fmt::init()` spustí výchozí subscriber, který loguje do stdout.

**`tower`** (dev-dependency) — abstrakce HTTP middlewaru. V testech potřebujeme metodu `.oneshot()` pro odeslání jednoho požadavku přímo do Axum routeru bez spouštění TCP serveru.

**`http-body-util`** (dev-dependency) — nástroje pro čtení HTTP těl v testech. Metoda `.collect().await` shromáždí streamed bajty do jednoho bufferu.

---

## Datové typy (PhoneNumber, Contact)

```rust
/// Jedno telefonní číslo s lidsky čitelným štítkem (např. "mobil", "práce").
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PhoneNumber {
    pub label: String,
    pub number: String,
}

/// Jeden kontakt — základní datová jednotka celé služby.
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
```

### Co dělá `#[derive(...)]`

`derive` je procedurální makro — Rust kompilátor ho při překladu rozbalí a vygeneruje implementaci daného traitu automaticky. Bez `derive` bychom museli každý trait implementovat ručně.

- **`Debug`** — umožňuje formátovat hodnotu pomocí `{:?}` nebo `{:#?}`. Potřebujeme to pro ladění a logování: `dbg!(contact)` nebo `format!("{:?}", contact)`. Bez `Debug` by kompilátor odmítl tyto výpisy.

- **`Clone`** — přidá metodu `.clone()`, která vytvoří hlubokou kopii hodnoty. V `search()` voláme `c.clone()` při sestavování výsledků, protože výsledky vlastní svá data — nelze vrátit reference do indexu, který drží zámek.

- **`Serialize`** — generuje kód pro převod hodnoty do formátu jako JSON. Axum volá `Serialize` automaticky, když handler vrátí `Json(contact)`.

- **`Deserialize`** — generuje kód pro vytvoření hodnoty z JSON. Axum volá `Deserialize`, když přijme `Json(body): Json<IndexBody>`.

### Co dělá `#[serde(default)]`

Bez `#[serde(default)]` by deserializace selhala s chybou, pokud by JSON neobsahoval daný klíč. S `#[serde(default)]` se místo chyby použije výchozí hodnota daného typu — pro `Vec<PhoneNumber>` je to prázdný vektor `[]`, pro `String` je to `""`.

```rust
// Toto JSON projde i bez "phones" a "category":
{
    "id": "42",
    "first_name": "Jana",
    "last_name": "Nováková",
    "email": "jana@example.com"
}
```

> **Poznámka:** `#[serde(default)]` je nutné aplikovat na každé pole zvlášť. `#[serde(default)]` na celou strukturu by použilo výchozí hodnotu celé struktury (vyžaduje implementaci traitu `Default`), ne jen chybějících polí.

---

## SearchIndex — datová struktura

```rust
const WEIGHT_LAST_NAME: f32 = 3.0;
const WEIGHT_FIRST_NAME: f32 = 2.0;
const WEIGHT_EMAIL: f32 = 1.0;
const WEIGHT_PHONE: f32 = 1.0;

#[derive(Debug, Default)]
pub struct SearchIndex {
    pub contacts: HashMap<String, Contact>,
    pub inverted_index: HashMap<String, Vec<(String, f32)>>,
}
```

### HashMap\<String, Vec\<(String, f32)\>\>

Klíčem je **token** — slovo vzniklé tokenizací (vždy malá písmena, jen alfanumerické znaky). Hodnotou je **posting list** — seznam dvojic `(contact_id, váha)`.

ASCII diagram:

```
inverted_index:

 "novak"   ──► [("id-123", 3.0), ("id-456", 1.0)]
                      │                  │
                      │                  └── "novak" je součást emailu id-456
                      └── "novak" je příjmení id-123

 "jana"    ──► [("id-123", 2.0)]
                      │
                      └── "jana" je křestní jméno id-123

 "example" ──► [("id-123", 1.0), ("id-456", 1.0)]
                      │                  │
                      └── oba mají @example.com
```

Při vyhledávání dotazu `"jana"`:
1. Vyhledáme klíč `"jana"` v `HashMap` — O(1).
2. Projdeme posting list: kontakt `id-123` dostane `+2.0` bodů.
3. Seřadíme výsledky sestupně a vrátíme kontakty.

### Proč oddělené `contacts` a `inverted_index`?

`inverted_index` ukládá jen ID a váhu — ne celý objekt `Contact`. Tím minimalizujeme paměť v posting listech (jeden kontakt může mít desítky tokenů). Teprve na konci `search()` přeložíme ID zpět na plné `Contact` objekty přes `self.contacts.get(cid)`.

### `#[derive(Default)]`

Generuje implementaci traitu `Default`, takže `SearchIndex::default()` vrátí strukturu s prázdnými `HashMap`y. `SearchIndex::new()` jen volá `Self::default()`.

---

## tokenize()

```rust
fn tokenize(s: &str) -> Vec<String> {
    s.to_lowercase()
        .split(|c: char| !c.is_alphanumeric())
        .filter(|t| !t.is_empty())
        .map(String::from)
        .collect()
}
```

Funkce bere `&str` (referenci na řetězec, žádná alokace při volání) a vrací `Vec<String>` (vlastněný vektor vlastněných řetězců).

### `.to_lowercase()` — unicode-aware

`to_lowercase()` v Rustu pracuje správně s unicode — `"Nováková".to_lowercase()` dá `"nováková"`, ne pokažené bajty. Interně používá unicode case-folding tabulky, takže pokryje i řeckou, arabskou nebo cyrylicovou abecedu.

### `.split(|c: char| !c.is_alphanumeric())` — closure jako predikát

`split` přijímá predikát — uzávěr (closure), který pro každý znak rozhodne, zda je to oddělovač. `!c.is_alphanumeric()` říká: odděl na každém znaku, který není písmeno ani číslice.

```
Vstup: "jana.novakova@example.com"
                 │       │
          tečka (.)  zavináč (@) jsou oddělovače

Výstup po split: ["jana", "novakova", "", "example", "com"]
                                       ↑
                              prázdný token mezi @ a dalším znakem
```

### `.filter(|t| !t.is_empty())` — proč filtrovat prázdné tokeny

Dvě sousední oddělovací znaky (např. `..` nebo mezerník za čárkou) produkují prázdný řetězec `""`. Kdybychom prázdné tokeny nevyfiltrovali, skončily by v indexu jako platné klíče a každý kontakt by je sdílel — při vyhledávání prázdného řetězce by vracely všechny výsledky.

> **Tip:** `tokenize` je symetrická — stejná funkce se volá při indexování i při vyhledávání. To zaručuje, že token z dotazu vždy odpovídá tokenu v indexu, bez potřeby speciálního normalizačního kroku.

---

## SearchIndex::rebuild()

```rust
pub fn rebuild(&mut self) {
    self.inverted_index.clear();

    for contact in self.contacts.values() {
        let id = contact.id.clone();

        let mut add_tokens = |text: &str, weight: f32| {
            for token in tokenize(text) {
                let entry = self
                    .inverted_index
                    .entry(token)
                    .or_default();

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
```

### Proč `self.inverted_index.clear()` na začátku

Vždy pracujeme s kompletním snapshotem kontaktů — klient posílá všechny kontakty najednou, ne jen změny. Kdybychom index jen appendovali, smazané kontakty by v něm zůstaly navždy. `clear()` na začátku zaručí, že po rebuildu index odpovídá přesně aktuálnímu stavu `self.contacts`.

### `entry().or_default()` — insert-or-update pattern

```rust
let entry = self.inverted_index.entry(token).or_default();
```

`entry(key)` vrátí `Entry` — enum, který reprezentuje buď obsazené místo (`Occupied`) nebo prázdné (`Vacant`). `.or_default()` pak:
- Pokud klíč existuje: vrátí mutable referenci na existující hodnotu.
- Pokud klíč neexistuje: vloží výchozí hodnotu (`Vec::new()`) a vrátí referenci na ni.

Toto je idiomatický Rust vzor pro "vlož, pokud neexistuje, a vždy vrať referenci". Alternativa s `if contains_key ... else insert` by vyžadovala dvě hashování klíče.

### Proč se váhy sčítají

Jedno slovo může patřit do více polí jednoho kontaktu. Kontakt `Jana Nováková` s emailem `novakova@firma.cz`:
- Token `"novakova"` vznikne z `last_name` (váha 3.0) i z `email` (váha 1.0).
- Výsledné skóre pro dotaz `"novakova"` je 4.0, ne 3.0.

Sčítání místo přepisování je správnější — kontakt se více "hodí" pro dotaz, pokud se slovo vyskytuje na více místech.

> **Poznámka pro borrow checker:** Closure `add_tokens` si půjčuje `self.inverted_index` mutabilně, ale zároveň čte `contact.id` (přes `id.clone()`). Rust by normálně odmítl sdílet `self` mutabilně i imutabilně ve stejný čas. Řešíme to tak, že `id` klonujeme před definicí closure — closure pak vlastní vlastní kopii `id` a nevyžaduje přístup k `self`.

---

## SearchIndex::search()

```rust
pub fn search(&self, query: &str) -> Vec<(Contact, f32)> {
    let tokens = tokenize(query);
    if tokens.is_empty() {
        return Vec::new();
    }

    let mut scores: HashMap<&str, f32> = HashMap::new();

    for token in &tokens {
        // --- Přesná shoda ---
        if let Some(postings) = self.inverted_index.get(token.as_str()) {
            for (cid, weight) in postings {
                *scores.entry(cid.as_str()).or_default() += weight;
            }
        }

        // --- Prefixová shoda ---
        for (indexed_token, postings) in &self.inverted_index {
            if indexed_token != token && indexed_token.starts_with(token.as_str()) {
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
            self.contacts.get(cid).map(|c| (c.clone(), score))
        })
        .collect();

    results.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
    results
}
```

### Krok po kroku

**1. Tokenizace dotazu**

```rust
let tokens = tokenize(query);
if tokens.is_empty() {
    return Vec::new();
}
```

Prázdný dotaz vrátí prázdný výsledek okamžitě. Bez téhle kontroly by prefixová smyčka níže procházela celý index a vracela všechno.

**2. Exact match větev**

```rust
if let Some(postings) = self.inverted_index.get(token.as_str()) {
    for (cid, weight) in postings {
        *scores.entry(cid.as_str()).or_default() += weight;
    }
}
```

`HashMap::get` je O(1). Pokud token v indexu existuje, projdeme jeho posting list a přičteme plnou váhu ke skóre každého dotčeného kontaktu. `*scores.entry(...).or_default() += weight` je zkratka pro "přidej 0.0 pokud kontakt ještě nemá skóre, pak přičti váhu".

**3. Prefix match větev**

```rust
for (indexed_token, postings) in &self.inverted_index {
    if indexed_token != token && indexed_token.starts_with(token.as_str()) {
        let prefix_weight = 0.5;
        for (cid, weight) in postings {
            *scores.entry(cid.as_str()).or_default() += weight * prefix_weight;
        }
    }
}
```

Procházíme celý index a hledáme tokeny, jejichž začátek odpovídá dotazu. `starts_with` je O(délka_prefixu). Celá smyčka je O(počet unikátních tokenů v indexu) — pro tisíce kontaktů stále pod 1 ms.

Podmínka `indexed_token != token` zabrání tomu, aby exact-match token byl zároveň zpracován jako prefix-match (s nižší váhou). Bez ní by přesná shoda dostala `plná_váha + 0.5 × plná_váha`, což by ji zkreslilo.

Penalizace `0.5` je záměrná: prefix-shoda je méně jistá než přesná. Uživatel píše "Nov" a hledá Nováková, ale přesná shoda "Jana" musí vždy předčit prefix "Jan", pokud by existoval jiný kontakt přesně jménem "Jan".

**4. Agregace scores**

`HashMap<&str, f32>` místo `HashMap<String, f32>` — klíče jsou `&str` ukazující přímo do `self.contacts`, takže nevznikají zbytečné alokace. Rust borrow checker to garantuje — `self` žije dost dlouho, dokud `scores` existuje.

**5. `sort_by` s `partial_cmp` — proč partial**

```rust
results.sort_by(|a, b| b.1.partial_cmp(&a.1).unwrap_or(std::cmp::Ordering::Equal));
```

`f32` implementuje `PartialOrd`, ne `Ord`. Důvod: `f32` může nabývat hodnoty `NaN` (Not a Number), a `NaN != NaN` — `NaN` není porovnatelné s ničím včetně sebe sama. Proto `partial_cmp` vrací `Option<Ordering>` místo `Ordering`.

V praxi se `NaN` v našem kódu nikdy nevyskytne (váhy jsou kladné konstanty), ale kompilátor o tom neví a vynutí ošetření. `.unwrap_or(Equal)` říká: pokud by přeci jen nastalo NaN, považuj ty dva prvky za stejně hodnotné.

Řadíme `b.1.partial_cmp(&a.1)` (b před a) — to je sestupné řazení (nejvyšší skóre první).

---

## AppState

```rust
#[derive(Clone)]
pub struct AppState {
    pub index: Arc<RwLock<SearchIndex>>,
    pub requests: Arc<AtomicU64>,
    pub errors: Arc<AtomicU64>,
    pub start_time: Instant,
}
```

### `Arc<RwLock<SearchIndex>>` — proč takto složitě?

Axum spouští handlery souběžně v thread poolu. Každý handler dostane klon `AppState` — ale `Clone` u `AppState` jen zvýší čítače referencí uvnitř `Arc`, nekopíruje data.

**`Arc`** (Atomically Reference Counted) zajišťuje, že `SearchIndex` žije tak dlouho, dokud na něj ukazuje alespoň jeden `Arc`. Jakmile všechny klony zaniknou, data se automaticky uvolní.

**`RwLock`** (Read-Write Lock) umožňuje:
- Neomezený počet souběžných čtení (`read().await`) — stovky vyhledávání najednou.
- Exkluzivní zápis (`write().await`) — jen jedno indexování v daný moment.

Používáme `tokio::sync::RwLock`, ne `std::sync::RwLock`. Tokio varianta je asynchronní — čekání na zámek pozastaví jen daný async task, ne celé OS vlákno. Ostatní handlery mohou běžet dál na stejném vlákně.

### `Arc<AtomicU64>` — lock-free čítač

```rust
pub requests: Arc<AtomicU64>,
pub errors: Arc<AtomicU64>,
```

Alternativou by byl `Arc<Mutex<u64>>`. Proč raději `AtomicU64`?

- `Mutex` zamykání je zbytečné pro prostou inkrementaci čísla. Zamykání, čekání, odemykání — to je overhead pro operaci, která trvá jeden CPU instrukci.
- `AtomicU64::fetch_add` je jedna atomická instrukce (`LOCK XADD` na x86). Žádný zámek, žádné blokování.

### `Ordering::Relaxed` — proč stačí

```rust
state.requests.fetch_add(1, Ordering::Relaxed);
```

`Ordering` říká procesoru a kompilátoru, jak silná musí být paměťová garance kolem atomické operace.

- `Ordering::SeqCst` — nejsilnější, garantuje globální pořadí všech atomických operací. Potřeba, pokud jiné vlákno musí vidět výsledek *před* tím, než provede závislou operaci.
- `Ordering::Relaxed` — nejslabší, garantuje jen atomicitu samotné operace. Bez zaručeného pořadí vůči jiným operacím.

Pro čítadla požadavků a chyb nás nezajímá přesné pořadí — jen chceme, aby hodnota nebyla ztracena. `Relaxed` je správná volba a je rychlejší než silnější varianty.

---

## HTTP handlery (Axum)

### POST /index — handle_index

```rust
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
```

**`State(state): State<AppState>`** — dependency injection v Axum. Axum handler je prostá async funkce; `State(...)` je extrakční vzor (destructuring) pro parametr typu `State<AppState>`. Axum automaticky předá stav registrovaný přes `.with_state(state)`. Žádný globální singleton, žádný `lazy_static` — stav teče přes typový systém.

**`Json(body): Json<IndexBody>`** — automatická deserializace. Axum přečte tělo požadavku, zavolá `serde_json::from_slice` a výsledek předá do `body`. Pokud JSON není validní nebo mu chybí povinná pole, Axum automaticky vrátí `422 Unprocessable Entity` — handler se ani nespustí.

**Návratový typ `Result<Json<Value>, (StatusCode, Json<Value>)>`** — Axum umí konvertovat `Result` na HTTP odpověď. `Ok(...)` vrátí `200 OK`, `Err((status, json))` vrátí dané status code s JSON tělem.

### GET /search — handle_search

```rust
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
```

**`Query(params): Query<SearchParams>`** — Axum parsuje query string `?q=jana` do struktury `SearchParams`. Pokud query string chybí nebo je malformed, vrátí `422`.

**`params.q.unwrap_or_default()`** — `q` je `Option<String>`, protože `/search` bez `?q=` je validní požadavek (vrátí prázdné výsledky). `.unwrap_or_default()` dá `String::new()` pro chybějící `q`.

**`state.index.read().await`** — sdílené čtení. Více vyhledávání může běžet paralelně. Čtecí zámek se uvolní automaticky na konci bloku, kdy `idx` přestane existovat (RAII).

**Skóre se do odpovědi nezahrne** — `.map(|(c, _score)| c)` zahodí skóre. Klient ho nepotřebuje; pořadí výsledků samo o sobě nese informaci o relevanci.

### GET /health a GET /stats

```rust
async fn handle_health() -> Json<Value> {
    Json(json!({ "status": "ok", "service": "search-rust" }))
}

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
```

`handle_health` nepotřebuje `State` — nekoukne na žádný sdílený stav, proto parametr vůbec nemá. Axum to zvládne bez problémů; handlery jsou prostě funkce.

`start_time: Instant` — hodnota není za `Arc`, protože `Instant` je `Copy` a nemění se. Klonuje se přímo do každé kopie `AppState`.

---

## Startup sekvence

```rust
async fn startup(_state: AppState, self_url: String) {
    let client = Client::new();

    let contacts_url = std::env::var("CONTACTS_URL")
        .unwrap_or_else(|_| "http://localhost:8080".to_string());
    let gateway_url = std::env::var("GATEWAY_URL")
        .unwrap_or_else(|_| "http://localhost:9000".to_string());

    // --- Krok 1: Načtení kontaktů ---
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
                    Err(e) => warn!("Failed to parse contacts response: {}", e),
                }
            }
            Ok(resp) => warn!("Contacts service returned {}", resp.status()),
            Err(e) => warn!("Could not reach contacts service: {}", e),
        }
        tokio::time::sleep(tokio::time::Duration::from_secs(2)).await;
    };

    // --- Krok 2: Self-index ---
    let index_endpoint = format!("{}/index", self_url);
    let body = json!({ "contacts": contacts });
    match client.post(&index_endpoint).json(&body).send().await {
        Ok(resp) => info!("Self-index response: {}", resp.status()),
        Err(e) => error!("Failed to self-index: {}", e),
    }

    // --- Krok 3: Registrace u gateway ---
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
```

### Retry loop — proč `loop` místo `for`

```rust
let contacts: Vec<Contact> = loop {
    // ...
    break list;   // úspěch
    // ...
    tokio::time::sleep(...).await;  // selhal, zkus znovu
};
```

`for i in 0..MAX` by vyžadovalo předem znát maximální počet pokusů a řešit případ, kdy všechny pokusy selžou. `loop { break value }` je idiomatický Rust pro "opakuj dokud nevyjde" — smyčka skončí pouze úspěchem, nikdy silentem.

`loop` navíc vrací hodnotu — `break list` předá `list` jako výsledek celého výrazu, takže `contacts` dostane přímo načtený vektor bez pomocných proměnných.

### `tokio::time::sleep` — async sleep, neblokuje vlákno

```rust
tokio::time::sleep(tokio::time::Duration::from_secs(2)).await;
```

`std::thread::sleep(...)` by zablokoval celé OS vlákno — ostatní async tasky na tom vlákně by nemohly běžet. `tokio::time::sleep(...).await` jen pozastaví tento konkrétní task a uvolní vlákno pro ostatní.

### Self-call na `/index` — proč přes HTTP a ne přímé volání metody

```rust
let index_endpoint = format!("{}/index", self_url);
client.post(&index_endpoint).json(&body).send().await;
```

Mohli bychom zavolat `state.index.write().await` a `idx.rebuild()` přímo. Záměrně to neděláme, protože:

1. **Testování celého stacku** — self-call projde přes HTTP parsing, Axum router, handler — stejnou cestu jako reálné požadavky. Chyba v deserializaci nebo routingu se odhalí hned při startu.
2. **Konzistence** — zaindexování při startu sdílí přesně stejnou logiku jako zaindexování z API. Neexistují dvě různé cesty, které by se mohly rozejít.

---

## main()

```rust
#[tokio::main]
async fn main() {
    tracing_subscriber::fmt::init();

    let state = AppState::new();
    let self_url = std::env::var("SELF_URL")
        .unwrap_or_else(|_| "http://localhost:8081".to_string());

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
```

### `#[tokio::main]`

Makro, které obalí `async fn main` do tokio asynchronního runtime. Bez něj by `async fn main` bylo syntakticky neplatné — Rust neumí spustit async funkci bez runtime, který ji naplánuje. Makro rozbalí přibližně na:

```rust
fn main() {
    tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .unwrap()
        .block_on(async {
            // ... tělo vaší async main ...
        })
}
```

### `tokio::spawn` pro startup background task

```rust
tokio::spawn(async move {
    startup(state_clone, self_url_clone).await;
});
```

`tokio::spawn` je ekvivalent spuštění vlákna, ale místo OS vlákna spustí "zelené vlákno" (green thread / coroutine). Je to řádově levnější než OS vlákno — tokio jich zvládne tisíce na malém počtu OS vláken.

`async move` — `move` přesune zachycené proměnné (`state_clone`, `self_url_clone`) do async bloku. Bez `move` by se block pokoušel půjčit proměnné ze scope `main`, které by mohly zaniknout dříve než task doběhne.

Blok `{}` kolem `spawn` zajistí, že `state_clone` a `self_url_clone` zaniknou před dalším použitím `state` a `self_url` — Rust borrow checker tím ví, že klony jsou nezávislé.

### `.with_state(state)` — předání stavu do handlerů

```rust
let app = Router::new()
    // ...
    .with_state(state);
```

Axum předá `state` do každého handleru, který má parametr `State<AppState>`. Interně Axum volá `Clone` pro každý požadavek — proto je `Clone` u `AppState` levná operace (jen zvýší `Arc` čítač).

### `TcpListener::bind` + `axum::serve`

```rust
let listener = tokio::net::TcpListener::bind("0.0.0.0:8081")
    .await
    .expect("Failed to bind to 0.0.0.0:8081");

axum::serve(listener, app).await.expect("Server error");
```

Oddělení `bind` od `serve` je záměrné — umožňuje testům svázat port, aniž by server začal přijímat spojení. `axum::serve` blokuje (`.await`) do fatální chyby serveru.

---

## Testy

Zdrojový kód obsahuje dvě skupiny testů v modulu `#[cfg(test)]`:

**Unit testy pro `SearchIndex`** — testují logiku bez HTTP:

```rust
#[test]
fn test_tokenize() {
    let tokens = tokenize("Jana Nováková");
    assert_eq!(tokens, vec!["jana", "nováková"]);
}

#[test]
fn test_prefix_match() {
    let mut index = SearchIndex::new();
    let c = make_contact("1", "Jana", "Nováková", "jana@example.com");
    index.contacts.insert(c.id.clone(), c);
    index.rebuild();

    let results = index.search("Nov");
    assert_eq!(results.len(), 1, "prefix 'Nov' should find Nováková");
}

#[test]
fn test_weight_last_name_higher_than_first_name() {
    // Kontakt B má "test" jako příjmení, kontakt A jako křestní jméno.
    // Po vyhledávání musí být B první (WEIGHT_LAST_NAME > WEIGHT_FIRST_NAME).
    // ...
}
```

**Integrační testy pro HTTP handlery** — používají Axum `oneshot` pro odeslání požadavku přímo do routeru bez TCP:

```rust
#[tokio::test]
async fn test_index_and_search() {
    let state = AppState::new();
    let app = Router::new()
        .route("/index", axum::routing::post(handle_index))
        .route("/search", axum::routing::get(handle_search))
        // ...
        .with_state(state);

    let index_resp = app.clone().oneshot(index_req).await.unwrap();
    assert_eq!(index_resp.status(), StatusCode::OK);

    let search_resp = app.oneshot(search_req).await.unwrap();
    // ...
}
```

`app.clone()` před `oneshot` — `oneshot` spotřebuje router, proto klonujeme pro druhý požadavek.

### Jak spustit testy

```bash
cd services/search-rust
cargo test
```

### Jak testovat ručně

```bash
# Spuštění serveru
cargo run

# Zaindexování kontaktů
curl -s -X POST http://localhost:8081/index \
  -H 'Content-Type: application/json' \
  -d '{"contacts":[{"id":"1","first_name":"Jana","last_name":"Nováková","email":"jana@example.com","phones":[],"category":""}]}'

# Vyhledávání — exact match
curl "http://localhost:8081/search?q=jana"

# Vyhledávání — prefix match (najde Nováková)
curl "http://localhost:8081/search?q=Nov"

# Health check
curl http://localhost:8081/health

# Statistiky
curl http://localhost:8081/stats
```

Očekávaný výstup search:

```json
{
  "results": [
    {
      "id": "1",
      "first_name": "Jana",
      "last_name": "Nováková",
      "email": "jana@example.com",
      "phones": [],
      "category": ""
    }
  ],
  "total": 1,
  "took_ms": 0
}
```

---

## Klíčové prvky jazyka Rust

### `Arc<RwLock<T>>` — sdílené vlastnictví + vnitřní mutabilita

```rust
pub index: Arc<RwLock<SearchIndex>>,
```

Dvě ortogonální problémy, dvě obálky:

**`Arc`** (Atomic Reference Counting) řeší sdílené vlastnictví. Rust jinak dovoluje pouze jednoho vlastníka — `Arc` umožňuje sdílet hodnotu mezi vlákny přes čítač referencí. Klonování `Arc` je levné (jen atomický přírůstek čítače), nezní se žádná data.

**`RwLock`** řeší přístup k datům. Rust jinak dovoluje buď N sdílených referencí `&T` nebo jednu exkluzivní `&mut T` — nikdy obojí. `RwLock` tyto kontroly přesune z compile time do runtime: `read()` vrátí sdílenou referenci (mohou běžet paralelně), `write()` vrátí exkluzivní referenci (blokuje všechna čtení).

`tokio::sync::RwLock` místo `std::sync::RwLock` — asynchronní varianta. Nečeká blokováním OS vlákna, ale pozastavením async tasku.

### `#[derive(...)]` — procedurální makra

```rust
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Contact { ... }
```

`#[derive]` je **procedurální makro** — generátor kódu spuštěný při kompilaci. Kompilátor předá AST (abstract syntax tree) struktury makru, makro vygeneruje implementaci traitu a vloží ji do zdrojového kódu.

| Trait | Co vygeneruje |
|---|---|
| `Debug` | `impl fmt::Debug` — výpis `{:?}` pro debugging |
| `Clone` | `impl Clone` — metoda `.clone()` pro kopírování |
| `Serialize` | `impl serde::Serialize` — konverze do JSON/YAML/... |
| `Deserialize` | `impl serde::Deserialize` — konverze z JSON/YAML/... |

Alternativa by bylo napsat implementaci ručně — desítky řádků boilerplate pro každou strukturu.

### `Option<T>` a `Result<T, E>` — algebraické typy

```rust
pub q: Option<String>,   // query parametr může chybět

async fn handle() -> Result<Json<Value>, (StatusCode, Json<Value>)>
```

**`Option<T>`** — buď `Some(hodnota)` nebo `None`. Nahrazuje `null` z jiných jazyků, ale bezpečně — kompilátor vynucuje ošetření obou variant.

**`Result<T, E>`** — buď `Ok(hodnota)` nebo `Err(chyba)`. Nahrazuje výjimky — chyba je součástí typového podpisu funkce, volající ji musí explicitně ošetřit.

Operátor `?` je syntaktický cukr:
```rust
let data = some_operation()?;
// ekvivalentní:
let data = match some_operation() {
    Ok(v) => v,
    Err(e) => return Err(e.into()),
};
```

### `impl Trait` — trait systém

```rust
impl SearchIndex {
    pub fn rebuild(&mut self, contacts: &[Contact]) { ... }
    pub fn search(&self, query: &str) -> Vec<Contact> { ... }
}
```

**Trait** je podobný interface v Go nebo abstract class v C++ — definuje sadu metod. `impl SearchIndex` implementuje metody přímo na strukturu (ne trait). `&self` = sdílená reference (čtení), `&mut self` = exkluzivní reference (zápis).

Rust nemá dědičnost — místo ní composition a traity. Chování se sdílí přes traity, ne přes třídy.
