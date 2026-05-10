#include <httplib.h>
#include <nlohmann/json.hpp>
#include <pqxx/pqxx>

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

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// UUID generator
// ---------------------------------------------------------------------------

// Generuje UUID verze 4 (náhodný) bez závislosti na OS nebo externích knihovnách.
// RFC 4122 vyžaduje specifické bity: verze (4) v nibble 13, varianta (8-9-a-b) v nibble 17.
// Mersenne Twister (mt19937) je dostatečně kvalitní PRNG pro identifikátory —
// kryptografická bezpečnost zde není potřeba.
static std::string generate_uuid() {
    // static = sdílené přes všechna volání, inicializované jen jednou (thread-safe v C++11+)
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    // Převádí náhodné 32bitové číslo na hex řetězec o `nibbles` znacích (nibble = 4 bity = 1 hex znak)
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

    // Verze 4: horní nibble nastavíme na 0100 (= 0x4xxx)
    uint32_t v = (dist(gen) & 0x0FFF) | 0x4000;
    // Varianta RFC 4122: dva horní bity nastavíme na 10 (= 0x8xxx–0xBxxx)
    uint32_t r = (dist(gen) & 0x3FFF) | 0x8000;

    // Stejná hexkonverze jako `hex`, ale bez closure přes dist (používá hotovou hodnotu)
    auto h = [](uint32_t x, int n) {
        std::string s;
        for (int i = n - 1; i >= 0; --i) {
            int b = (x >> (i * 4)) & 0xF;
            s += (b < 10) ? char('0' + b) : char('a' + b - 10);
        }
        return s;
    };

    // Formát UUID: 8-4-4-4-12 hexadecimálních znaků oddělených pomlčkami
    return hex(8) + "-" + hex(4) + "-" + h(v, 4) + "-" + h(r, 4) + "-" + hex(8) + hex(4);
}

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

// Telefonní číslo s popiskem (např. "mobil", "work")
struct PhoneNumber { std::string label, number; };

// Jeden kontakt; `phones` je 1:N vztah (více čísel na kontakt)
struct Contact {
    std::string id, first_name, last_name, email, category;
    std::vector<PhoneNumber> phones;
};

// Serializuje kontakt do JSON objektu pro odpovědi API
static json contact_to_json(const Contact& c) {
    json phones = json::array();
    for (const auto& p : c.phones)
        phones.push_back({{"label", p.label}, {"number", p.number}});
    return {{"id", c.id}, {"first_name", c.first_name}, {"last_name", c.last_name},
            {"email", c.email}, {"phones", phones}, {"category", c.category}};
}

// Deserializuje kontakt z JSON; pokud `id` není předáno, vygeneruje nové UUID.
// `j.value(key, default)` vrátí default místo výjimky, pokud klíč chybí.
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

// ---------------------------------------------------------------------------
// PostgreSQL connection pool
// ---------------------------------------------------------------------------

// Sdílený pool připojení k databázi — otevírání nového spojení trvá desítky ms,
// proto je výrazně rychlejší recyklovat existující připojení mezi požadavky.
class ConnPool {
    std::string dsn_;
    mutable std::mutex mtx_;               // chrání přístup k `pool_` z více vláken
    mutable std::condition_variable cv_;   // probouzí čekající vlákna když se připojení vrátí
    mutable std::queue<std::unique_ptr<pqxx::connection>> pool_;

public:
    // RAII guard: při zániku objektu automaticky vrátí připojení zpět do poolu.
    // Díky tomu se na `release()` nemusí explicitně pamatovat ani v cestách s výjimkou.
    struct Guard {
        ConnPool* pool;
        std::unique_ptr<pqxx::connection> conn;
        ~Guard() { if (conn) pool->release(std::move(conn)); }
        pqxx::connection& get() { return *conn; }
    };

    // Vytvoří `n` připojení dopředu — typicky 8 odpovídá počtu HTTP worker vláken
    ConnPool(const std::string& dsn, size_t n = 8) : dsn_(dsn) {
        for (size_t i = 0; i < n; ++i)
            pool_.push(std::make_unique<pqxx::connection>(dsn));
    }

    // Vyzvedne jedno volné připojení; pokud žádné není, zablokuje vlákno dokud se neuvolní.
    // `unique_lock` + `cv_.wait` je standardní C++ vzor pro čekání na podmínku bez busy-loop.
    Guard acquire() const {
        std::unique_lock lk(mtx_);
        cv_.wait(lk, [this] { return !pool_.empty(); });
        auto c = std::move(pool_.front());
        pool_.pop();
        return Guard{const_cast<ConnPool*>(this), std::move(c)};
    }

    // Vrátí připojení do poolu a probudí jedno čekající vlákno (notify_one = probudí jen jedno,
    // což je efektivnější než notify_all, protože stejně může pokračovat jen jedno)
    void release(std::unique_ptr<pqxx::connection> c) const {
        std::unique_lock lk(mtx_);
        pool_.push(std::move(c));
        cv_.notify_one();
    }
};

// ---------------------------------------------------------------------------
// ContactStore — PostgreSQL backed
// ---------------------------------------------------------------------------

// Datová vrstva aplikace: všechny operace s kontakty jdou přes tuto třídu.
// Interně používá ConnPool, takže je bezpečná pro volání z více vláken zároveň.
class ContactStore {
    ConnPool pool_;

    // Převede jeden databázový řádek na struct Contact (bez telefonních čísel)
    static Contact row_to_contact(const pqxx::row& r) {
        Contact c;
        c.id         = r["id"].as<std::string>();
        c.first_name = r["first_name"].as<std::string>();
        c.last_name  = r["last_name"].as<std::string>();
        // email může být NULL v DB — is_null() guard zabrání výjimce při konverzi
        c.email      = r["email"].is_null() ? "" : r["email"].as<std::string>();
        c.category   = r["category"].as<std::string>();
        return c;
    }

    // Donačte telefonní čísla pro daný kontakt v rámci existující transakce.
    // Separátní dotaz (místo JOIN) zjednodušuje mapování 1:N na straně C++.
    void load_phones(pqxx::work& tx, Contact& c) const {
        auto pr = tx.exec_params(
            "SELECT label, number FROM phone_numbers WHERE contact_id = $1 ORDER BY id",
            c.id);
        for (const auto& r : pr)
            c.phones.push_back({r[0].as<std::string>(), r[1].as<std::string>()});
    }

    // Vloží telefonní čísla pro kontakt; volá se uvnitř transakce přidání/aktualizace
    void insert_phones(pqxx::work& tx, const std::string& id,
                       const std::vector<PhoneNumber>& phones) {
        for (const auto& p : phones)
            tx.exec_params0(
                "INSERT INTO phone_numbers(contact_id, label, number) VALUES($1,$2,$3)",
                id, p.label, p.number);
    }

public:
    // Připojí se k databázi, vytvoří tabulky pokud neexistují a načte seed data.
    // Konstruktor blokuje dokud DB není připravena — záměrně, aplikace nemá smysl bez DB.
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

        // Seed pokud je tabulka prázdná — zajistí že demo prostředí má s čím pracovat
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
            seed("Jana",  "Nováková", "jana@example.com",      "Friend",
                 {{"mobil", "+420 601 111 222"}});
            seed("Petr",  "Svoboda",  "petr.svoboda@work.cz",  "Colleague",
                 {{"work",  "+420 602 333 444"}});
            seed("Marie", "Horáková", "marie.horakova@home.cz","Family",
                 {{"home",  "+420 603 555 666"}, {"mobil", "+420 777 888 999"}});
            seed("Tomáš", "Dvořák",   "tomas@example.com",     "Other", {});
        }
        tx.commit();
    }

    // Přidá nový kontakt do DB (včetně telefonních čísel) v jedné transakci
    void add(const Contact& c) {
        auto g = pool_.acquire();
        pqxx::work tx(g.get());
        tx.exec_params0(
            "INSERT INTO contacts(id,first_name,last_name,email,category) VALUES($1,$2,$3,$4,$5)",
            c.id, c.first_name, c.last_name, c.email, c.category);
        insert_phones(tx, c.id, c.phones);
        tx.commit();
    }

    // Vrátí všechny kontakty; pokud je `q` neprázdné, filtruje full-text LIKE přes více polí.
    // Vyhledávání probíhá na straně DB (ne v C++), aby byl přenos dat minimální.
    std::vector<Contact> get_all(const std::string& q = "") const {
        auto g = pool_.acquire();
        pqxx::work tx(g.get());
        pqxx::result rows;
        if (q.empty()) {
            rows = tx.exec(
                "SELECT id,first_name,last_name,email,category FROM contacts "
                "ORDER BY last_name, first_name");
        } else {
            // Konkatenace polí do jednoho řetězce před LIKE — hledá i přes hranice polí
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

    // Vrátí kontakt podle ID; `std::optional` vyjadřuje že kontakt nemusí existovat
    // — čistší než výjimka nebo speciální "prázdný" Contact s id=""
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

    // Aktualizuje kontakt; telefony se nejdřív smažou a vloží znovu — jednodušší než diff
    bool update(const std::string& id, const Contact& u) {
        auto g = pool_.acquire();
        pqxx::work tx(g.get());
        auto r = tx.exec_params(
            "UPDATE contacts SET first_name=$2,last_name=$3,email=$4,category=$5 WHERE id=$1",
            id, u.first_name, u.last_name, u.email, u.category);
        if (r.affected_rows() == 0) return false;   // kontakt s tímto ID neexistuje
        tx.exec_params0("DELETE FROM phone_numbers WHERE contact_id=$1", id);
        insert_phones(tx, id, u.phones);
        tx.commit();
        return true;
    }

    // Smaže kontakt; ON DELETE CASCADE v DB se postará o smazání telefonních čísel
    bool remove(const std::string& id) {
        auto g = pool_.acquire();
        pqxx::work tx(g.get());
        auto r = tx.exec_params("DELETE FROM contacts WHERE id=$1", id);
        tx.commit();
        return r.affected_rows() > 0;
    }

    // ── Fuzzy dedup ───────────────────────────────────────────────────────

    // Levenshteinova vzdálenost: minimální počet jednoznakových operací (vložení,
    // smazání, nahrazení) pro převod řetězce `a` na `b`.
    // Algoritmus používá dynamické programování — dp[i][j] = vzdálenost prefixů a[0..i] a b[0..j].
    // Časová i paměťová složitost je O(m*n), kde m,n jsou délky řetězců.
    static int levenshtein(const std::string& a, const std::string& b) {
        size_t m = a.size(), n = b.size();
        std::vector<std::vector<int>> dp(m+1, std::vector<int>(n+1));
        // Základní případ: převod na/z prázdného řetězce = počet znaků
        for (size_t i = 0; i <= m; ++i) dp[i][0] = i;
        for (size_t j = 0; j <= n; ++j) dp[0][j] = j;
        for (size_t i = 1; i <= m; ++i)
            for (size_t j = 1; j <= n; ++j)
                // Pokud znaky jsou stejné, nepřidáváme žádnou operaci;
                // jinak vezmeme minimum ze tří operací (smazání, vložení, nahrazení)
                dp[i][j] = (a[i-1] == b[j-1]) ? dp[i-1][j-1]
                          : 1 + std::min({dp[i-1][j], dp[i][j-1], dp[i-1][j-1]});
        return dp[m][n];
    }

    // Převede Levenshteinovu vzdálenost na podobnost v rozsahu [0.0, 1.0].
    // Normalizuje délkou delšího jména, takže krátká i dlouhá jména jsou srovnatelná.
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

    // Najde potenciální duplikáty — dvojice kontaktů jejichž podobnost jmen překračuje `threshold`.
    // Porovnává každou dvojici jednou (j > i), tedy O(n²) — pro velké adresáře by bylo potřeba
    // efektivnější přístup (např. blocking nebo LSH).
    json dedup(double threshold) const {
        auto contacts = get_all();
        std::vector<json> pairs;
        for (size_t i = 0; i < contacts.size(); ++i) {
            for (size_t j = i + 1; j < contacts.size(); ++j) {
                double sim = name_similarity(contacts[i], contacts[j]);
                if (sim >= threshold)
                    pairs.push_back({
                        {"contact_a",  contact_to_json(contacts[i])},
                        {"contact_b",  contact_to_json(contacts[j])},
                        // round*1000/1000 zaokrouhlí na 3 desetinná místa bez závislosti na printf formátu
                        {"similarity", std::round(sim * 1000) / 1000},
                    });
            }
        }
        // std::ranges::sort (C++20): přehlednější zápis než std::sort s iterátory
        std::ranges::sort(pairs, [](const json& a, const json& b) {
            return a["similarity"].get<double>() > b["similarity"].get<double>();
        });
        return {{"pairs", json(pairs)}, {"total", pairs.size()}, {"threshold", threshold}};
    }

    // ── Analytics ─────────────────────────────────────────────────────────

    // Sestaví agregované statistiky adresáře v jednom průchodu přes kontakty
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
                    domains[c.email.substr(at + 1)]++;   // část za @ = doména
            }
            for (const auto& p : c.phones)
                phone_labels[p.label]++;
        }

        // Top 10 domén sestupně — std::views::take (C++20 ranges) efektivně ořízne bez kopírování
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

    // ── vCard export ──────────────────────────────────────────────────────

    // Exportuje všechny kontakty ve formátu vCard 3.0 (RFC 2426).
    // Oddělovač řádků je \r\n (CRLF) — vyžaduje to standard vCard.
    std::string export_vcard() const {
        auto contacts = get_all();
        std::ostringstream out;
        for (const auto& c : contacts) {
            out << "BEGIN:VCARD\r\nVERSION:3.0\r\n";
            out << "FN:" << c.first_name << " " << c.last_name << "\r\n";
            // N: pole má formát Příjmení;Jméno;Prostřední;Titul;Suffix — prázdné části = ;;;
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

    // ── vCard import ──────────────────────────────────────────────────────

    // Parsuje vCard data (mohou obsahovat více karet) a uloží kontakty do DB.
    // Jednoduchý stavový stroj: přepíná mezi stavem "uvnitř karty" a "mimo kartu".
    int import_vcard(const std::string& data) {
        int imported = 0;
        std::istringstream ss(data);
        std::string line;
        Contact current;
        bool in_card = false;

        // Odstraní bílé znaky z obou konců řetězce — nutné kvůli \r na konci řádků a mezerám
        auto trim = [](std::string s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c){ return !std::isspace(c); }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
            return s;
        };

        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();  // normalizace CRLF → LF
            line = trim(line);
            if (line == "BEGIN:VCARD") {
                current = Contact{};
                current.id = generate_uuid();
                current.category = "Other";
                in_card = true;
            } else if (line == "END:VCARD" && in_card) {
                // Kontakt je platný jen pokud má alespoň jedno jméno
                if (!current.first_name.empty() || !current.last_name.empty()) {
                    // Fallback: pokud chybí křestní jméno, použijeme příjmení jako celé jméno
                    if (current.first_name.empty()) current.first_name = current.last_name;
                    add(current);
                    imported++;
                }
                in_card = false;
            } else if (in_card) {
                auto colon = line.find(':');
                if (colon == std::string::npos) continue;   // přeskočíme nerozpoznané řádky
                std::string key = line.substr(0, colon);
                std::string val = trim(line.substr(colon + 1));
                if (key == "FN") {
                    // FN = celé zobrazované jméno; rozdělíme na první slovo a zbytek
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
                    // Popisek telefonu je zakódovaný v klíči, např. "TEL;TYPE=mobil"
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

    // Vrátí surová data obou tabulek (contacts + phone_numbers) — pro debugging a admin UI
    json raw_tables() const {
        auto g = pool_.acquire();
        pqxx::work tx(g.get());

        auto cr = tx.exec(
            "SELECT id, first_name, last_name, email, category FROM contacts ORDER BY last_name, first_name");
        json contacts = json::array();
        for (const auto& r : cr)
            contacts.push_back({{"id", r[0].as<std::string>()},
                                {"first_name", r[1].as<std::string>()},
                                {"last_name", r[2].as<std::string>()},
                                {"email", r[3].is_null() ? "" : r[3].as<std::string>()},
                                {"category", r[4].as<std::string>()}});

        auto pr = tx.exec(
            "SELECT id, contact_id, label, number FROM phone_numbers ORDER BY contact_id, id");
        json phones = json::array();
        for (const auto& r : pr)
            phones.push_back({{"id", r[0].as<long long>()},
                              {"contact_id", r[1].as<std::string>()},
                              {"label", r[2].as<std::string>()},
                              {"number", r[3].as<std::string>()}});

        tx.commit();
        return {{"contacts", contacts}, {"phone_numbers", phones}};
    }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Přečte proměnnou prostředí, nebo vrátí výchozí hodnotu — bez try/catch díky getenv API
static std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

// Nastaví HTTP status a tělo odpovědi jako JSON — centralizováno aby se Content-Type neopakoval
static void json_response(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

// Asynchronně notifikuje search službu o novém/upraveném kontaktu.
// Detached thread: nechceme blokovat HTTP odpověď kvůli pomalé síti; pokud notifikace selže,
// ignorujeme to — search index je "best effort", ne kritická data.
static void notify_search(const std::string& search_url, const Contact& c) {
    std::thread([search_url, c] {
        try {
            httplib::Client cli(search_url);
            cli.set_connection_timeout(2);   // krátký timeout: search je best-effort
            cli.set_read_timeout(2);
            auto body = contact_to_json(c).dump();
            cli.Post("/index", body, "application/json");
        } catch (...) {}
    }).detach();
}

// Pokusí se zaregistrovat u API gateway s opakováním — gateway se může startovat souběžně
static void register_with_gateway(const std::string& gateway_url) {
    json reg = {{"name", "contacts"}, {"url", "http://contacts-cpp:8080"}, {"health_path", "/health"}};
    for (int i = 1; i <= 5; ++i) {
        try {
            httplib::Client cli(gateway_url);
            cli.set_connection_timeout(3);
            cli.set_read_timeout(3);
            auto res = cli.Post("/services", reg.dump(), "application/json");
            if (res && res->status < 300) {
                std::cout << std::format("[gateway] registered on attempt {}\n", i);
                return;
            }
        } catch (const std::exception& e) {
            std::cout << std::format("[gateway] attempt {} failed: {}\n", i, e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    std::cerr << "[gateway] registration failed\n";
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    const std::string gateway_url = env_or("GATEWAY_URL", "http://localhost:9000");
    const std::string search_url  = env_or("SEARCH_URL",  "http://localhost:8081");
    const std::string db_url      = env_or("DATABASE_URL",
        "postgresql://contacts:contacts@localhost:5432/contacts");

    std::cout << "[contacts-cpp] connecting to PostgreSQL...\n";
    ContactStore store(db_url);
    std::cout << "[contacts-cpp] database ready\n";

    // std::atomic zajišťuje thread-safe čítání bez mutex zámku — vhodné pro čítače které
    // se pouze inkrementují z více vláken
    std::atomic<uint64_t> req_count{0}, err_count{0};
    const auto start_time = std::chrono::steady_clock::now();

    // Registrace probíhá v odděleném vlákně po 1s — dáme serveru čas spustit se dříve
    // než gateway pošle health check. Detach = nepotřebujeme čekat na výsledek.
    std::thread([gateway_url] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        register_with_gateway(gateway_url);
    }).detach();

    httplib::Server svr;
    // Thread pool s 8 vlákny = paralelní zpracování požadavků; odpovídá velikosti DB poolu
    svr.new_task_queue = [] { return new httplib::ThreadPool(8); };

    // GET /health — pro health check gateway a orchestrátoru (Docker, k8s)
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, {{"status","ok"},{"service","contacts-cpp"},{"threads",8}});
    });

    // GET /stats — operační metriky: počty požadavků a uptime
    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
        json_response(res, 200, {{"requests",req_count.load()},{"errors",err_count.load()},{"uptime_s",uptime}});
    });

    // GET /contacts?q=... — seznam kontaktů s volitelným fulltextovým filtrem
    svr.Get("/contacts", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        std::string q = req.has_param("q") ? req.get_param_value("q") : "";
        auto contacts = store.get_all(q);
        json arr = json::array();
        for (const auto& c : contacts) arr.push_back(contact_to_json(c));
        json_response(res, 200, arr);
    });

    // GET /contacts/:id — regex v cestě zachytí ID kontaktu do req.matches[1]
    svr.Get(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        auto opt = store.get_by_id(req.matches[1]);
        if (!opt) { ++err_count; json_response(res, 404, {{"error","not found"}}); return; }
        json_response(res, 200, contact_to_json(*opt));
    });

    // POST /contacts — vytvoří nový kontakt; ID se vygeneruje server-side pro zajištění unikátnosti
    svr.Post("/contacts", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        try {
            auto j = json::parse(req.body);
            Contact c = contact_from_json(j, generate_uuid());
            if (c.first_name.empty() || c.last_name.empty()) {
                ++err_count;
                json_response(res, 400, {{"error","first_name and last_name required"}});
                return;
            }
            store.add(c);
            notify_search(search_url, c);
            json_response(res, 201, contact_to_json(c));
        } catch (const json::exception& e) {
            ++err_count;
            json_response(res, 400, {{"error", std::string("invalid JSON: ") + e.what()}});
        }
    });

    // PUT /contacts/:id — plná náhrada kontaktu (ID z URL přebíjí ID v těle)
    svr.Put(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        const std::string id = req.matches[1];
        try {
            auto j = json::parse(req.body);
            Contact u = contact_from_json(j, id);
            if (u.first_name.empty() || u.last_name.empty()) {
                ++err_count;
                json_response(res, 400, {{"error","first_name and last_name required"}});
                return;
            }
            if (!store.update(id, u)) {
                ++err_count;
                json_response(res, 404, {{"error","not found"}});
                return;
            }
            notify_search(search_url, u);
            json_response(res, 200, contact_to_json(u));
        } catch (const json::exception& e) {
            ++err_count;
            json_response(res, 400, {{"error", std::string("invalid JSON: ") + e.what()}});
        }
    });

    // DELETE /contacts/:id — 204 No Content při úspěchu, 404 pokud kontakt neexistuje
    svr.Delete(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        if (!store.remove(req.matches[1])) {
            ++err_count;
            json_response(res, 404, {{"error","not found"}});
            return;
        }
        json_response(res, 204, json(nullptr));
    });

    // GET /db/tables — surová data DB pro ladění; v produkci by měl být chráněn autorizací
    svr.Get("/db/tables", [&](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, store.raw_tables());
    });

    // GET /dedup?threshold=0.85 — hledá potenciální duplikáty; threshold je skóre podobnosti [0,1]
    svr.Get("/dedup", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        double threshold = 0.85;
        if (req.has_param("threshold")) {
            try { threshold = std::stod(req.get_param_value("threshold")); }
            catch (...) {}
        }
        // std::clamp zajistí že threshold zůstane v platném rozsahu i při neplatném vstupu
        threshold = std::clamp(threshold, 0.0, 1.0);
        json_response(res, 200, store.dedup(threshold));
    });

    // GET /analytics — agregované statistiky adresáře (kategorie, domény, popisky telefonů)
    svr.Get("/analytics", [&](const httplib::Request&, httplib::Response& res) {
        ++req_count;
        json_response(res, 200, store.analytics());
    });

    // GET /export/vcard — stáhne všechny kontakty jako .vcf soubor (vCard 3.0)
    svr.Get("/export/vcard", [&](const httplib::Request&, httplib::Response& res) {
        ++req_count;
        res.status = 200;
        // Content-Disposition: attachment způsobí stažení souboru místo zobrazení v prohlížeči
        res.set_header("Content-Disposition", "attachment; filename=\"contacts.vcf\"");
        res.set_content(store.export_vcard(), "text/vcard; charset=utf-8");
    });

    // POST /import/vcard — přijme .vcf soubor v těle požadavku a importuje kontakty
    svr.Post("/import/vcard", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        int n = store.import_vcard(req.body);
        json_response(res, 200, {{"imported", n}});
    });

    std::cout << "[contacts-cpp] listening on 0.0.0.0:8080\n";
    svr.listen("0.0.0.0", 8080);
    return 0;
}
