# contacts-cpp — Průvodce kódem

## Přehled

`contacts-cpp` je autoritativní zdroj dat o kontaktech v systému polycontacts. Implementuje kompletní REST API nad PostgreSQL databází — od CRUD operací přes fuzzy deduplikaci až po import/export ve formátu vCard — vše v jediném C++20 souboru `main.cpp`.

---

## Includes a závislosti

```cpp
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>
```

Tři externí knihovny tvoří celý základ služby:

- **`httplib.h`** — header-only HTTP server (cpp-httplib). Celý server v jediném `.h` souboru, žádné linkování. Podporuje thread pool a regex routes.
- **`nlohmann/json.hpp`** — header-only JSON knihovna. Umožňuje psát `json j = {{"key", value}}` jako by to byl Python dict. Opět žádné linkování.
- **`pqxx/pqxx`** — libpqxx, C++ wrapper nad libpq (oficíální PostgreSQL klientská knihovna). Přidává RAII transakce, parametrizované dotazy a typově bezpečné čtení sloupců.

```cpp
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <ranges>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
```

Ze standardní knihovny:

| Header | K čemu slouží |
|---|---|
| `<algorithm>` | `std::min`, `std::transform`, `std::clamp` |
| `<atomic>` | Lock-free čítače požadavků a chyb |
| `<chrono>` | Měření uptime, timeouty HTTP klienta |
| `<condition_variable>` | Čekání na volné DB spojení v connection poolu |
| `<format>` | C++20 typově bezpečná náhrada `printf` |
| `<map>` | Agregace statistik (kategorie → počet) |
| `<mutex>` | Ochrana sdílené fronty spojení |
| `<optional>` | Návratová hodnota `get_by_id` — může existovat nebo ne |
| `<queue>` | Fronta volných DB spojení v poolu |
| `<random>` | Mersenne Twister pro generování UUID |
| `<ranges>` | C++20 ranges — `std::ranges::sort`, `std::views::take` |
| `<sstream>` | Sestavení vCard výstupu, parsing vCard vstupu |
| `<thread>` | Background vlákna pro registraci u gateway a notify_search |

```cpp
using json = nlohmann::json;
```

Alias `json` zkracuje zápis `nlohmann::json` na jediné slovo — používá se v celém souboru.

---

## UUID generátor

```cpp
static std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    auto hex = [&](int nibbles) {
        uint32_t val = dist(gen);
        std::string s;
        s.reserve(nibbles);
        for (int i = nibbles - 1; i >= 0; --i) {
            int n = (val >> (i * 4)) & 0xF;
            s += (n < 10) ? char('0' + n) : char('a' + n - 10);
        }
        return s;
    };

    uint32_t v = (dist(gen) & 0x0FFF) | 0x4000;
    uint32_t r = (dist(gen) & 0x3FFF) | 0x8000;

    auto h = [](uint32_t x, int n) {
        std::string s;
        for (int i = n - 1; i >= 0; --i) {
            int b = (x >> (i * 4)) & 0xF;
            s += (b < 10) ? char('0' + b) : char('a' + b - 10);
        }
        return s;
    };

    return hex(8) + "-" + hex(4) + "-" + h(v, 4) + "-" + h(r, 4) + "-" + hex(8) + hex(4);
}
```

### Proč `static` u PRNG?

```cpp
static std::random_device rd;
static std::mt19937 gen(rd());
static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
```

Klíčové slovo `static` uvnitř funkce znamená: proměnná se inicializuje **jednou**, při prvním volání funkce, a pak přetrvává po celou dobu běhu programu. V C++11 a novějším je tato inicializace **thread-safe** — standard garantuje, že pokud dvě vlákna zavolají funkci současně, inicializace proběhne přesně jednou, bez data race.

Bez `static` by se `gen` (stav Mersenne Twisteru) vytvářel znovu při každém UUID, což je pomalé a zároveň by pokaždé začínal ze stejného seed stavu z `rd()` — výsledky by byly méně náhodné.

### Co je nibble?

Nibble (také nybble) je **4 bity**, tedy polovina bajtu. Jeden hexadecimální znak (0–F) reprezentuje právě jeden nibble — proto `nibbles` ve funkci znamená „počet hex znaků".

Extrakce nibble z hodnoty:

```cpp
int n = (val >> (i * 4)) & 0xF;
```

- `i * 4` — posun o `i` nibblů doleva
- `>> (i * 4)` — přesun požadovaného nibble na nejnižší pozici
- `& 0xF` — maskování, ponechá jen spodní 4 bity (jeden nibble)

### RFC 4122 — version bity a variant bity

UUID verze 4 (náhodné) má podle RFC 4122 dvě povinná pole:

```cpp
// Version: nibble 13 musí být 0100 (= hodnota 4 = verze 4)
uint32_t v = (dist(gen) & 0x0FFF) | 0x4000;
//            ^^^^^^^^^^^^^^^^         ^^^^^^
//            12 náhodných bitů   horní nibble pevně 0x4 (0100)

// Variant: horní 2 bity nibble 17 musí být 10 (= hodnota 8–B)
uint32_t r = (dist(gen) & 0x3FFF) | 0x8000;
//            ^^^^^^^^^^^^^^^^         ^^^^^^
//            14 náhodných bitů   horní 2 bity pevně 10
```

UUID má formát `xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`, kde:

- pozice `4` říká: toto UUID je verze 4 (náhodné)
- pozice `y` je hodnota 8, 9, a nebo b (variant bity = `10xx`)

### Proč Mersenne Twister a ne `/dev/urandom`?

`/dev/urandom` (nebo `std::random_device`) je kryptograficky bezpečný zdroj náhodnosti ze systému — ale každé čtení je syscall, tedy pomalé. `std::mt19937` (Mersenne Twister) je pseudonáhodný generátor: inicializuje se jednou z `rd()` a pak generuje miliony čísel bez jediného syscallu.

Pro identifikátory kontaktů kryptografická bezpečnost není potřeba — UUID slouží jen k jednoznačné identifikaci záznamu, ne k ochraně hesla. Mersenne Twister je dostatečně kvalitní (perioda 2^19937 − 1) a výrazně rychlejší.

!!! note "Formát výsledku"
    `hex(8) + "-" + hex(4) + "-" + h(v, 4) + "-" + h(r, 4) + "-" + hex(8) + hex(4)`
    produkuje UUID ve standardním formátu `8-4-4-4-12`, například:
    `f47ac10b-58cc-4372-a567-0e02b2c3d479`

---

## Datový model (PhoneNumber, Contact, JSON helpers)

### Struktury

```cpp
struct PhoneNumber { std::string label, number; };

struct Contact {
    std::string id, first_name, last_name, email, category;
    std::vector<PhoneNumber> phones;
};
```

`Contact` je plochá datová struktura — obsahuje jen hodnoty, žádné metody. Vztah 1:N (jeden kontakt, více telefonů) je modelován jako `std::vector<PhoneNumber>`. Tato jednoduchost usnadňuje serializaci i přenos přes vlákna.

### Serializace do JSON

```cpp
static json contact_to_json(const Contact& c) {
    json phones = json::array();
    for (const auto& p : c.phones)
        phones.push_back({{"label", p.label}, {"number", p.number}});
    return {{"id", c.id}, {"first_name", c.first_name}, {"last_name", c.last_name},
            {"email", c.email}, {"phones", phones}, {"category", c.category}};
}
```

nlohmann/json umožňuje inicializovat JSON objekt přes initializer list `{{"key", value}, ...}` — syntaxe je záměrně podobná Pythonu nebo JavaScriptu. Telefony jsou vnořené pole objektů.

### Deserializace z JSON

```cpp
static Contact contact_from_json(const json& j, const std::string& id = "") {
    Contact c;
    c.id         = id.empty() ? j.value("id", generate_uuid()) : id;
    c.first_name = j.value("first_name", "");
    c.last_name  = j.value("last_name", "");
    c.email      = j.value("email", "");
    c.category   = j.value("category", "Other");
    if (j.contains("phones") && j["phones"].is_array())
        for (const auto& p : j["phones"])
            c.phones.push_back({p.value("label", ""), p.value("number", "")});
    return c;
}
```

### Proč `j.value(key, default)` místo `j[key]`?

`j["key"]` hodí výjimku `json::out_of_range` pokud klíč v JSON objektu neexistuje. `j.value("key", default_value)` místo toho vrátí výchozí hodnotu — takže `contact_from_json` funguje i pro neúplné JSON payloady (například klient pošle kontakt bez `category`, dostane výchozí hodnotu `"Other"`).

!!! tip "Pořadí ID"
    Parametr `id` je volitelný. Když ho předáme (při PUT požadavku), přepíše cokoliv co by JSON obsahoval — ID z URL vždy vyhraje nad tělem. Když ho nepředáme, funkce zkusí vzít `id` z JSON, nebo vygeneruje nové UUID.

---

## Connection Pool (ConnPool)

Otevření nového PostgreSQL spojení trvá desítky milisekund — navazuje se TCP spojení, autentizace, negotiace protokolu. Pro server s 8 worker vlákny by to znamenalo latenci navíc při každém požadavku. `ConnPool` udržuje sadu předem otevřených spojení a recykluje je.

```cpp
class ConnPool {
    std::string dsn_;
    mutable std::mutex mtx_;
    mutable std::condition_variable cv_;
    mutable std::queue<std::unique_ptr<pqxx::connection>> pool_;

public:
    struct Guard {
        ConnPool* pool;
        std::unique_ptr<pqxx::connection> conn;
        ~Guard() { if (conn) pool->release(std::move(conn)); }
        pqxx::connection& get() { return *conn; }
    };

    ConnPool(const std::string& dsn, size_t n = 8) : dsn_(dsn) {
        for (size_t i = 0; i < n; ++i)
            pool_.push(std::make_unique<pqxx::connection>(dsn));
    }

    Guard acquire() const {
        std::unique_lock lk(mtx_);
        cv_.wait(lk, [this] { return !pool_.empty(); });
        auto c = std::move(pool_.front());
        pool_.pop();
        return Guard{const_cast<ConnPool*>(this), std::move(c)};
    }

    void release(std::unique_ptr<pqxx::connection> c) const {
        std::unique_lock lk(mtx_);
        pool_.push(std::move(c));
        cv_.notify_one();
    }
};
```

### Proč pool?

TCP spojení je drahé. Měřením se ukazuje, že `pqxx::connection()` trvá typicky 20–50 ms. Bez poolu by každý HTTP požadavek nesl tuto latenci navíc. S poolem jsou spojení otevřena jednou při startu a pak volně přiřazována vláknům.

### `condition_variable` + `unique_lock` — klasický C++ vzor

```cpp
Guard acquire() const {
    std::unique_lock lk(mtx_);                         // (1) zamkne mutex
    cv_.wait(lk, [this] { return !pool_.empty(); });   // (2) čeká, odemyká mutex při čekání
    auto c = std::move(pool_.front());                 // (3) vezme spojení
    pool_.pop();
    return Guard{const_cast<ConnPool*>(this), std::move(c)};
}
```

`cv_.wait(lk, predikát)` funguje takto:

1. Zkontroluje predikát — je fronta neprázdná?
2. Pokud ano: pokračuje dál (drží zámek).
3. Pokud ne: odemkne mutex, uspí vlákno (skutečné uspání, ne busy-loop), počká na signál.
4. Při signálu: opět zamkne mutex, znovu ověří predikát (ochrana před spurious wakeup).

`unique_lock` (oproti `lock_guard`) umožňuje dočasné odemknutí, které `condition_variable::wait` potřebuje.

### RAII Guard — proč destruktor?

```cpp
struct Guard {
    ConnPool* pool;
    std::unique_ptr<pqxx::connection> conn;
    ~Guard() { if (conn) pool->release(std::move(conn)); }
};
```

RAII (Resource Acquisition Is Initialization) znamená: prostředek se získá při konstrukci, uvolní při destrukci. Destruktor se volá **vždy** — i když funkce skončí výjimkou.

Bez Guard by bylo nutné psát:

```cpp
// BEZ RAII — nebezpečný kód
auto conn = pool.borrow();
pqxx::work tx(conn);
tx.exec(...);        // ← co když toto hodí výjimku?
pool.return(conn);   // ← toto se NIKDY nezavolá
// spojení navždy zmizí z poolu → pool se postupně vyčerpá
```

S Guard se spojení vrátí automaticky i při výjimce — pool nikdy nevyčerpáme.

### `notify_one` vs `notify_all`

```cpp
void release(std::unique_ptr<pqxx::connection> c) const {
    std::unique_lock lk(mtx_);
    pool_.push(std::move(c));
    cv_.notify_one();   // probudí jen jedno čekající vlákno
}
```

`notify_one` probudí **jedno** čekající vlákno. `notify_all` by probudilo všechna — ale stejně může pokračovat jen jedno (jedno spojení se vrátilo). Ostatní by se probudila, zjistila prázdnou frontu a zase usnula — zbytečná práce. `notify_one` je proto efektivnější.

---

## ContactStore — schéma a seed data

```cpp
explicit ContactStore(const std::string& dsn) : pool_(dsn) {
    auto g = pool_.acquire();
    pqxx::work tx(g.get());
    tx.exec0(R"SQL(
        CREATE TABLE IF NOT EXISTS contacts (
            id         TEXT PRIMARY KEY,
            first_name TEXT NOT NULL,
            last_name  TEXT NOT NULL,
            email      TEXT,
            category   TEXT NOT NULL DEFAULT 'Other'
        );
        CREATE TABLE IF NOT EXISTS phone_numbers (
            id         BIGSERIAL PRIMARY KEY,
            contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE,
            label      TEXT NOT NULL,
            number     TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_contacts_sort ON contacts(last_name, first_name);
    )SQL");

    auto cnt = tx.exec1("SELECT COUNT(*) FROM contacts")[0].as<int>();
    if (cnt == 0) {
        auto seed = [&](const char* fn, const char* ln, const char* email,
                        const char* cat,
                        std::vector<std::pair<const char*, const char*>> phones) {
            std::string id = generate_uuid();
            tx.exec_params0(
                "INSERT INTO contacts(id,first_name,last_name,email,category) VALUES($1,$2,$3,$4,$5)",
                id, fn, ln, email, cat);
            for (auto& [lbl, num] : phones)
                tx.exec_params0(
                    "INSERT INTO phone_numbers(contact_id,label,number) VALUES($1,$2,$3)",
                    id, lbl, num);
        };
        seed("Jana",  "Nováková", "jana@example.com",      "Friend",    {{"mobil", "+420 601 111 222"}});
        seed("Petr",  "Svoboda",  "petr.svoboda@work.cz",  "Colleague", {{"work",  "+420 602 333 444"}});
        seed("Marie", "Horáková", "marie.horakova@home.cz","Family",    {{"home",  "+420 603 555 666"}, {"mobil", "+420 777 888 999"}});
        seed("Tomáš", "Dvořák",   "tomas@example.com",     "Other",     {});
    }
    tx.commit();
}
```

### `CREATE TABLE IF NOT EXISTS` — idempotentní

Příkaz `CREATE TABLE IF NOT EXISTS` selže tiše, pokud tabulka už existuje — místo vyhození chyby. To znamená, že konstruktor `ContactStore` lze volat bezpečně i při restartu kontejneru, kdy databáze data již má. Schéma se provede při každém startu, ale pokud tabulky existují, nestane se nic.

### ON DELETE CASCADE — proč důležité

```sql
contact_id TEXT NOT NULL REFERENCES contacts(id) ON DELETE CASCADE
```

`ON DELETE CASCADE` říká PostgreSQL: pokud smažeš kontakt z tabulky `contacts`, automaticky smaž i všechny jeho záznamy v `phone_numbers`. Alternativou by bylo explicitní `DELETE FROM phone_numbers WHERE contact_id = $1` před každým `DELETE FROM contacts` — `CASCADE` tuto povinnost přenáší na databázi, čímž eliminuje možnost zapomenout.

!!! warning "Pořadí je důležité"
    Bez `CASCADE` by `DELETE FROM contacts WHERE id = $1` selhal s chybou cizího klíče, pokud by pro kontakt existovala telefonní čísla. `CASCADE` zaručuje konzistenci automaticky.

### Proč seed data v C++ kódu a ne v SQL migraci?

Seed data jsou zde záměrně inline — tato služba nemá migrační systém (Flyway, Liquibase). Alternativa by byla samostatný `.sql` soubor, ale pak by bylo potřeba ho načíst z disku, zajistit že existuje, řešit cestu. Inline lambda `seed()` je jednodušší pro demo prostředí, kde chceme mít data "hned po startu bez dalšího nastavení". V produkci by seed data patřila do SQL migrace.

---

## ContactStore — CRUD metody

### add

```cpp
void add(const Contact& c) {
    auto g = pool_.acquire();
    pqxx::work tx(g.get());
    tx.exec_params0(
        "INSERT INTO contacts(id,first_name,last_name,email,category) VALUES($1,$2,$3,$4,$5)",
        c.id, c.first_name, c.last_name, c.email, c.category);
    insert_phones(tx, c.id, c.phones);
    tx.commit();
}
```

`exec_params0` spustí dotaz s parametry `$1`–`$5` a nevrátí žádné řádky (suffix `0` = zero rows expected). Kontakt a jeho telefony se vloží v jedné transakci — pokud `insert_phones` selže, celé `INSERT INTO contacts` se rollbackuje. Konzistence dat je zachována.

### Parametrizace `$1`, `$2` — ochrana před SQL injection

```cpp
tx.exec_params0(
    "INSERT INTO contacts(...) VALUES($1,$2,$3,$4,$5)",
    c.id, c.first_name, c.last_name, c.email, c.category);
```

Parametry `$1`–`$N` jsou **placeholdery** — libpqxx je předá PostgreSQL odděleně od SQL textu. Databáze přijme SQL dotaz a hodnoty jako dva samostatné pakety: dotaz se parsuje jako šablona, hodnoty se nikdy neinterpretují jako SQL. To znamená, že žádná hodnota (ani `'; DROP TABLE contacts; --`) nemůže změnit strukturu dotazu.

!!! warning "SQL injection"
    Přímé skládání řetězců `"INSERT ... VALUES('" + name + "')"` je nebezpečné — pokud `name` obsahuje uvozovku nebo SQL klíčové slovo, může útočník spustit libovolný SQL. Parametrizované dotazy jsou jedinou správnou obranou.

### get_all

```cpp
std::vector<Contact> get_all(const std::string& q = "") const {
    auto g = pool_.acquire();
    pqxx::work tx(g.get());
    pqxx::result rows;
    if (q.empty()) {
        rows = tx.exec(
            "SELECT id,first_name,last_name,email,category FROM contacts "
            "ORDER BY last_name, first_name");
    } else {
        rows = tx.exec_params(
            "SELECT id,first_name,last_name,email,category FROM contacts "
            "WHERE lower(first_name||' '||last_name||' '||coalesce(email,'')||' '||category) "
            "LIKE lower($1) ORDER BY last_name, first_name",
            "%" + q + "%");
    }
    std::vector<Contact> result;
    for (const auto& r : rows) {
        auto c = row_to_contact(r);
        load_phones(tx, c);
        result.push_back(std::move(c));
    }
    tx.commit();
    return result;
}
```

SQL full-text filtr konkatenuje všechna textová pole do jednoho řetězce a prohledává ho jedním `LIKE`. `coalesce(email,'')` zajistí, že NULL email nezpůsobí NULL výsledek celé konkatenace.

**Proč separátní dotaz pro telefony (ne JOIN)?**

Místo `JOIN phone_numbers` se volá `load_phones(tx, c)` pro každý kontakt zvlášť. Důvod: JOIN vrací kartézský součin — kontakt se třemi telefony by se objevil jako tři řádky, přičemž data kontaktu by se opakovala. Mapování takových řádků zpět na `Contact` (jeden objekt se třemi `PhoneNumber`) by vyžadovalo trackování "aktuálního" kontaktu a detekci změny ID. Separátní dotaz je čitelnější a pro typické adresáře (stovky kontaktů) dostatečně rychlý.

### get_by_id

```cpp
std::optional<Contact> get_by_id(const std::string& id) const {
    auto g = pool_.acquire();
    pqxx::work tx(g.get());
    auto rows = tx.exec_params(
        "SELECT id,first_name,last_name,email,category FROM contacts WHERE id=$1", id);
    if (rows.empty()) return std::nullopt;
    auto c = row_to_contact(rows[0]);
    load_phones(tx, c);
    tx.commit();
    return c;
}
```

`std::optional<Contact>` explicitně vyjadřuje, že kontakt nemusí existovat. Volající musí zkontrolovat `if (!opt)` — kompilátor ho k tomu přinutí. Alternativa (vrátit `Contact` s prázdným `id`) by byla mlčenlivý smluvní kód, který lze snadno přehlédnout.

### update

```cpp
bool update(const std::string& id, const Contact& u) {
    auto g = pool_.acquire();
    pqxx::work tx(g.get());
    auto r = tx.exec_params(
        "UPDATE contacts SET first_name=$2,last_name=$3,email=$4,category=$5 WHERE id=$1",
        id, u.first_name, u.last_name, u.email, u.category);
    if (r.affected_rows() == 0) return false;
    tx.exec_params0("DELETE FROM phone_numbers WHERE contact_id=$1", id);
    insert_phones(tx, id, u.phones);
    tx.commit();
    return true;
}
```

**Proč DELETE + INSERT pro telefony?**

Při aktualizaci kontaktu může klient přidat, odebrat nebo změnit telefony. Výpočet rozdílu (diff) — co přibylo, co zmizelo, co se změnilo — by byl složitý kód. Jednoduší přístup: smaž všechny telefony pro daný kontakt a vlož nové ze vstupu. V jedné transakci je výsledek stejný, kód je podstatně kratší.

`r.affected_rows() == 0` detekuje, zda `UPDATE` opravdu něco změnil. Pokud ne, kontakt s daným ID neexistuje → vrátíme `false` → HTTP handler vrátí 404.

### remove

```cpp
bool remove(const std::string& id) {
    auto g = pool_.acquire();
    pqxx::work tx(g.get());
    auto r = tx.exec_params("DELETE FROM contacts WHERE id=$1", id);
    tx.commit();
    return r.affected_rows() > 0;
}
```

Stačí smazat kontakt z `contacts` — `ON DELETE CASCADE` se postará o automatické smazání z `phone_numbers`. Výsledek `affected_rows() > 0` říká, zda byl nějaký řádek skutečně smazán.

---

## Fuzzy deduplikace (Levenshtein)

### Levenshteinova vzdálenost

```cpp
static int levenshtein(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    std::vector<std::vector<int>> dp(m+1, std::vector<int>(n+1));

    // Základní případ
    for (size_t i = 0; i <= m; ++i) dp[i][0] = i;
    for (size_t j = 0; j <= n; ++j) dp[0][j] = j;

    for (size_t i = 1; i <= m; ++i)
        for (size_t j = 1; j <= n; ++j)
            dp[i][j] = (a[i-1] == b[j-1])
                ? dp[i-1][j-1]
                : 1 + std::min({dp[i-1][j], dp[i][j-1], dp[i-1][j-1]});

    return dp[m][n];
}
```

**DP tabulka — base case a přechody:**

`dp[i][j]` = minimální počet operací pro převod prvních `i` znaků řetězce `a` na prvních `j` znaků řetězce `b`.

Base case:
- `dp[i][0] = i` — převod `a[0..i]` na prázdný řetězec = smazání `i` znaků
- `dp[0][j] = j` — převod prázdného řetězce na `b[0..j]` = vložení `j` znaků

Přechod:
- Pokud `a[i-1] == b[j-1]`: znaky jsou stejné, žádná operace → `dp[i-1][j-1]`
- Jinak: vezmeme minimum ze tří operací:
  - `dp[i-1][j] + 1` — smazání znaku z `a`
  - `dp[i][j-1] + 1` — vložení znaku do `a`
  - `dp[i-1][j-1] + 1` — nahrazení znaku

!!! note "Příklad"
    `levenshtein("Jan", "Jana")` = 1 (vložení jednoho znaku).
    `levenshtein("Novak", "Novák")` = 1 (nahrazení `a` za `á`).

### Normalizace na interval [0, 1]

```cpp
static double name_similarity(const Contact& a, const Contact& b) {
    auto norm = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };
    std::string na = norm(a.first_name + " " + a.last_name);
    std::string nb = norm(b.first_name + " " + b.last_name);
    int dist = levenshtein(na, nb);
    int maxlen = std::max(na.size(), nb.size());
    return maxlen == 0 ? 1.0 : 1.0 - (double)dist / maxlen;
}
```

Levenshteinova vzdálenost je absolutní číslo — `dist("Jan", "Jana") = 1`, ale `dist("Jan Novák", "Jan Nováček") = 3`. Aby byly výsledky srovnatelné pro různě dlouhá jména, normalizujeme délkou delšího jména:

```
podobnost = 1.0 - (vzdálenost / max_délka)
```

Výsledek je v rozsahu [0.0, 1.0]: 1.0 = identická jména, 0.0 = zcela odlišná.

### Proč `round * 1000 / 1000`?

```cpp
{"similarity", std::round(sim * 1000) / 1000}
```

Floating-point aritmetika může vrátit `0.8499999999999` místo `0.85`. Zaokrouhlení na 3 desetinná místa odstraní tento šum ve výstupu JSON bez závislosti na formátovacích funkcích (`printf("%.3f")`). Funguje takto: `0.849999 * 1000 = 849.999 → round = 850 → 850/1000 = 0.85`.

### O(n²) — kdy to přestane stačit?

```cpp
for (size_t i = 0; i < contacts.size(); ++i)
    for (size_t j = i + 1; j < contacts.size(); ++j) {
        double sim = name_similarity(contacts[i], contacts[j]);
        ...
    }
```

Každou dvojici porovnáváme jednou (`j > i`), tedy celkem `n*(n-1)/2` porovnání. Každé `levenshtein()` má složitost O(m*n) kde m,n jsou délky jmen (řádově 20 znaků → ~400 operací).

Pro 1 000 kontaktů: 500 000 párů × 400 operací = 200 milionů operací. Pro 10 000 kontaktů to je 200× víc. Při takových objemech by bylo potřeba přejít na efektivnější přístupy jako **blocking** (seskupit kontakty podle prvního písmene a porovnávat jen v rámci skupin) nebo **LSH** (Locality-Sensitive Hashing).

---

## Analytics

```cpp
json analytics() const {
    auto contacts = get_all();

    std::map<std::string, int> cats, domains, phone_labels;
    int no_email = 0;
    for (const auto& c : contacts) {
        cats[c.category]++;
        if (c.email.empty()) {
            no_email++;
        } else {
            auto at = c.email.find('@');
            if (at != std::string::npos)
                domains[c.email.substr(at + 1)]++;
        }
        for (const auto& p : c.phones)
            phone_labels[p.label]++;
    }

    std::vector<std::pair<std::string,int>> dom_vec(domains.begin(), domains.end());
    std::ranges::sort(dom_vec, [](const auto& a, const auto& b){ return a.second > b.second; });
    json top_domains = json::array();
    for (auto& [k,v] : dom_vec | std::views::take(10))
        top_domains.push_back({{"domain", k}, {"count", v}});

    return {
        {"total",         (int)contacts.size()},
        {"no_email",      no_email},
        {"category_dist", cats},
        {"top_domains",   top_domains},
        {"phone_labels",  phone_labels},
    };
}
```

### `std::views::take(10)` — C++20 lazy range

```cpp
for (auto& [k,v] : dom_vec | std::views::take(10))
```

`std::views::take(10)` je **lazy view** — nevytváří nový vektor, nekopíruje data. Operátor `|` (pipe) aplikuje view na kontejner; iterátor se zastaví po 10 prvcích. Alternativa by byla explicitní for s podmínkou `if (i++ >= 10) break` — `views::take` je čitelnější a přenositelnější (snadno vyměnitelné za `take(5)` nebo `take(100)`).

Structured bindings `auto& [k,v]` rozbalí `std::pair<std::string,int>` na pojmenované proměnné — bez `.first` a `.second`.

---

## vCard export/import

### Export

```cpp
std::string export_vcard() const {
    auto contacts = get_all();
    std::ostringstream out;
    for (const auto& c : contacts) {
        out << "BEGIN:VCARD\r\nVERSION:3.0\r\n";
        out << "FN:" << c.first_name << " " << c.last_name << "\r\n";
        out << "N:" << c.last_name << ";" << c.first_name << ";;;\r\n";
        if (!c.email.empty())
            out << "EMAIL;TYPE=INTERNET:" << c.email << "\r\n";
        for (const auto& p : c.phones)
            out << "TEL;TYPE=" << p.label << ":" << p.number << "\r\n";
        if (!c.category.empty())
            out << "CATEGORIES:" << c.category << "\r\n";
        out << "UID:" << c.id << "\r\n";
        out << "END:VCARD\r\n";
    }
    return out.str();
}
```

**vCard formát (RFC 6350):**

vCard je textový formát pro výměnu kontaktních informací. Každá karta je ohraničena `BEGIN:VCARD` / `END:VCARD`. Nejdůležitější pole:

- `FN:` — Formatted Name, zobrazované jméno (povinné)
- `N:` — strukturované jméno ve formátu `Příjmení;Jméno;Prostřední;Titul;Suffix`. Prázdné části se oddělí středníky (proto `;;;` na konci)
- `TEL;TYPE=mobil:+420 601 111 222` — telefonní číslo s typem zakódovaným v parametru
- `UID:` — jednoznačný identifikátor karty (naše UUID)

**Proč `\r\n`?**

RFC 6350 (vCard 4.0) i RFC 2426 (vCard 3.0) explicitně vyžadují CRLF (`\r\n`) jako oddělovač řádků — historicky kvůli kompatibilitě se starými mail servery a protokoly. Importéry v Outlooku, Apple Kontaktech a jiných aplikacích očekávají CRLF; plain LF může způsobit nesprávné parsování.

### Import — stavový stroj

```cpp
int import_vcard(const std::string& data) {
    int imported = 0;
    std::istringstream ss(data);
    std::string line;
    Contact current;
    bool in_card = false;

    auto trim = [](std::string s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(),
            [](unsigned char c){ return !std::isspace(c); }));
        s.erase(std::find_if(s.rbegin(), s.rend(),
            [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
        return s;
    };

    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(line);
        if (line == "BEGIN:VCARD") {
            current = Contact{};
            current.id = generate_uuid();
            current.category = "Other";
            in_card = true;
        } else if (line == "END:VCARD" && in_card) {
            if (!current.first_name.empty() || !current.last_name.empty()) {
                if (current.first_name.empty()) current.first_name = current.last_name;
                add(current);
                imported++;
            }
            in_card = false;
        } else if (in_card) {
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = trim(line.substr(colon + 1));
            if (key == "FN") {
                auto sp = val.find(' ');
                if (sp != std::string::npos) {
                    current.first_name = val.substr(0, sp);
                    current.last_name  = val.substr(sp + 1);
                } else {
                    current.last_name = val;
                }
            } else if (key.starts_with("EMAIL")) {
                current.email = val;
            } else if (key.starts_with("TEL")) {
                std::string label = "tel";
                auto type_pos = key.find("TYPE=");
                if (type_pos != std::string::npos)
                    label = key.substr(type_pos + 5);
                current.phones.push_back({label, val});
            } else if (key == "CATEGORIES") {
                current.category = val;
            }
        }
    }
    return imported;
}
```

**Proč `in_card` flag?**

Soubor `.vcf` může obsahovat více karet za sebou. Parser musí vědět, zda aktuálně parsuje obsah karty nebo mezeru mezi kartami. Příznak `in_card` tvoří jednoduchý **dvoustavový automat**:

```
[mimo kartu] --BEGIN:VCARD--> [uvnitř karty] --END:VCARD--> [mimo kartu]
```

Řádky mimo kartu se ignorují. Tím je parser robustní vůči prázdným řádkům nebo komentářům mezi kartami.

`trim()` odstraní bílé znaky z obou konců. Je nutná kvůli normalizaci CRLF → LF (`\r` zůstane na konci řádku po `std::getline` na Windows souborech), ale také kvůli mezerám na začátku řádku, které vCard standard někdy používá pro "folding" dlouhých řádků.

`key.starts_with("TEL")` (C++20) zachytí i varianty jako `TEL;TYPE=mobil` nebo `TEL;TYPE=CELL` — klíč v vCard může mít parametry oddělené středníkem, ale vždy začíná slovem `TEL`.

---

## HTTP server a endpointy

```cpp
int main() {
    const std::string gateway_url = env_or("GATEWAY_URL", "http://localhost:9000");
    const std::string search_url  = env_or("SEARCH_URL",  "http://localhost:8081");
    const std::string db_url      = env_or("DATABASE_URL",
        "postgresql://contacts:contacts@localhost:5432/contacts");

    ContactStore store(db_url);

    std::atomic<uint64_t> req_count{0}, err_count{0};
    const auto start_time = std::chrono::steady_clock::now();

    std::thread([gateway_url] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        register_with_gateway(gateway_url);
    }).detach();

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(8); };

    svr.Get("/health", ...);
    svr.Get("/stats", ...);
    svr.Get("/contacts", ...);
    svr.Get(R"(/contacts/([^/]+))", ...);
    svr.Post("/contacts", ...);
    svr.Put(R"(/contacts/([^/]+))", ...);
    svr.Delete(R"(/contacts/([^/]+))", ...);
    svr.Get("/db/tables", ...);
    svr.Get("/dedup", ...);
    svr.Get("/analytics", ...);
    svr.Get("/export/vcard", ...);
    svr.Post("/import/vcard", ...);

    svr.listen("0.0.0.0", 8080);
}
```

### `httplib::ThreadPool(8)` — proč 8?

```cpp
svr.new_task_queue = [] { return new httplib::ThreadPool(8); };
```

8 vláken = 8 souběžných HTTP požadavků. Toto číslo záměrně odpovídá velikosti `ConnPool` (také 8 spojení). Kdybychom měli 16 vláken a 8 DB spojení, polovina vláken by blokovala na `acquire()`. Kdybychom měli 8 vláken a 16 spojení, polovina spojení by nikdy nebyla použita. Symetrie 8/8 je optimální pro toto použití.

### `std::atomic<uint64_t>` — proč ne mutex pro čítače?

```cpp
std::atomic<uint64_t> req_count{0}, err_count{0};
// ...
++req_count;  // v každém HTTP handleru
```

`std::atomic` zaručuje, že inkrementace je **atomická** — nelze ji přerušit uprostřed jiným vláknem. Implementuje se pomocí speciálních CPU instrukcí (`lock xadd` na x86) bez potřeby mutex zámku.

Mutex je těžší mechanismus: `lock()` → kritická sekce → `unlock()`. Pro prostou inkrementaci čítače je `atomic` řádově rychlejší a čitelnější.

### `notify_search` — detached thread (fire and forget)

```cpp
static void notify_search(const std::string& search_url, const Contact& c) {
    std::thread([search_url, c] {
        try {
            httplib::Client cli(search_url);
            cli.set_connection_timeout(2);
            cli.set_read_timeout(2);
            auto body = contact_to_json(c).dump();
            cli.Post("/index", body, "application/json");
        } catch (...) {}
    }).detach();
}
```

Notifikace search služby probíhá v **odděleném vlákně** — HTTP odpověď klientovi se nebrzdí pomalým voláním jiné služby. `.detach()` znamená, že hlavní vlákno na toto vlákno nečeká. Výjimky jsou pohlteny prázdným `catch (...)` — search index je "best effort", selhání notifikace není kritická chyba.

Lambda zachytí `search_url` a `c` **hodnotou** (`[search_url, c]`) — kopie jsou nutné, protože originální hodnoty mohou přestat existovat dříve než vlákno dobíhá.

### Registrace u gateway — proč background thread s delay?

```cpp
std::thread([gateway_url] {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    register_with_gateway(gateway_url);
}).detach();
```

Gateway a contacts-cpp se spouštějí souběžně v Docker Compose. Při startu systému gateway nemusí být ještě připravena přijímat registrace. Delay 1 sekundy dá serveru čas spustit se a začít naslouchat na portu 8080 — teprve pak se gateway doví o naší existenci.

`register_with_gateway` zkouší registraci 5krát s 2s přestávkami — celkem až 10 sekund čekání. Pokud se registrace nezdaří, je loggována chyba, ale server pokračuje v provozu.

### Regex routes

```cpp
svr.Get(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
    auto opt = store.get_by_id(req.matches[1]);
    ...
});
```

`R"(...)"` je raw string literal — zpětná lomítka se neinterpretují. `([^/]+)` je capture group: zachytí jeden nebo více znaků které nejsou lomítkem. Výsledek je dostupný v `req.matches[1]` (nultý match je celá cesta).

Registrace endpointů:

| Metoda | Pattern | `req.matches[1]` |
|---|---|---|
| `GET /contacts/abc-123` | `R"(/contacts/([^/]+))"` | `"abc-123"` |
| `PUT /contacts/abc-123` | `R"(/contacts/([^/]+))"` | `"abc-123"` |
| `DELETE /contacts/abc-123` | `R"(/contacts/([^/]+))"` | `"abc-123"` |

---

## Jak buildovat a testovat

```bash
# Sestavení a spuštění jen contacts-cpp (a jeho závislostí - PostgreSQL, gateway)
cd services && docker compose up --build -d contacts-cpp

# Základní operace
curl http://localhost:8080/contacts
curl http://localhost:8080/contacts?q=Jana

# Vytvoření kontaktu
curl -X POST http://localhost:8080/contacts \
  -H "Content-Type: application/json" \
  -d '{"first_name":"Eva","last_name":"Procházková","email":"eva@example.com","category":"Friend","phones":[{"label":"mobil","number":"+420 700 800 900"}]}'

# Aktualizace (nahraď ID skutečným UUID z odpovědi výše)
curl -X PUT http://localhost:8080/contacts/<id> \
  -H "Content-Type: application/json" \
  -d '{"first_name":"Eva","last_name":"Procházková","email":"eva.nova@example.com","category":"Friend","phones":[]}'

# Smazání
curl -X DELETE http://localhost:8080/contacts/<id>

# Fuzzy deduplikace
curl "http://localhost:8080/dedup?threshold=0.8"

# Analytics
curl http://localhost:8080/analytics

# Export vCard
curl http://localhost:8080/export/vcard -o contacts.vcf

# Import vCard
curl -X POST http://localhost:8080/import/vcard \
  --data-binary @contacts.vcf

# Diagnostika
curl http://localhost:8080/health
curl http://localhost:8080/stats
curl http://localhost:8080/db/tables
```

!!! tip "Logy při startu"
    `docker compose logs -f contacts-cpp` zobrazí průběh inicializace:
    ```
    [contacts-cpp] connecting to PostgreSQL...
    [contacts-cpp] database ready
    [contacts-cpp] listening on 0.0.0.0:8080
    [gateway] registered on attempt 1
    ```

!!! note "API tabulka"
    | Metoda | Cesta | Popis | Status |
    |---|---|---|---|
    | `GET` | `/contacts?q=` | Seznam kontaktů, volitelně filtrovaný | 200 |
    | `GET` | `/contacts/{id}` | Jeden kontakt | 200 / 404 |
    | `POST` | `/contacts` | Vytvoří kontakt, vygeneruje UUID | 201 |
    | `PUT` | `/contacts/{id}` | Přepíše kontakt | 200 / 404 |
    | `DELETE` | `/contacts/{id}` | Smaže kontakt + telefony (CASCADE) | 204 / 404 |
    | `GET` | `/dedup?threshold=` | Potenciální duplicity (výchozí 0.85) | 200 |
    | `GET` | `/analytics` | Statistiky kategorií, domén, telefonů | 200 |
    | `GET` | `/export/vcard` | Export jako `.vcf` soubor | 200 |
    | `POST` | `/import/vcard` | Import vCard souboru | 200 |
    | `GET` | `/db/tables` | Surová data obou tabulek (debug) | 200 |
    | `GET` | `/health` | Health check | 200 |
    | `GET` | `/stats` | Počty požadavků, chyb, uptime | 200 |
