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

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------
struct PhoneNumber { std::string label, number; };
struct Contact {
    std::string id, first_name, last_name, email, category;
    std::vector<PhoneNumber> phones;
};

static json contact_to_json(const Contact& c) {
    json phones = json::array();
    for (const auto& p : c.phones)
        phones.push_back({{"label", p.label}, {"number", p.number}});
    return {{"id", c.id}, {"first_name", c.first_name}, {"last_name", c.last_name},
            {"email", c.email}, {"phones", phones}, {"category", c.category}};
}

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

// ---------------------------------------------------------------------------
// ContactStore — PostgreSQL backed
// ---------------------------------------------------------------------------
class ContactStore {
    ConnPool pool_;

    static Contact row_to_contact(const pqxx::row& r) {
        Contact c;
        c.id         = r["id"].as<std::string>();
        c.first_name = r["first_name"].as<std::string>();
        c.last_name  = r["last_name"].as<std::string>();
        c.email      = r["email"].is_null() ? "" : r["email"].as<std::string>();
        c.category   = r["category"].as<std::string>();
        return c;
    }

    void load_phones(pqxx::work& tx, Contact& c) const {
        auto pr = tx.exec_params(
            "SELECT label, number FROM phone_numbers WHERE contact_id = $1 ORDER BY id",
            c.id);
        for (const auto& r : pr)
            c.phones.push_back({r[0].as<std::string>(), r[1].as<std::string>()});
    }

    void insert_phones(pqxx::work& tx, const std::string& id,
                       const std::vector<PhoneNumber>& phones) {
        for (const auto& p : phones)
            tx.exec_params0(
                "INSERT INTO phone_numbers(contact_id, label, number) VALUES($1,$2,$3)",
                id, p.label, p.number);
    }

public:
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

        // seed pokud je prázdné
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

    void add(const Contact& c) {
        auto g = pool_.acquire();
        pqxx::work tx(g.get());
        tx.exec_params0(
            "INSERT INTO contacts(id,first_name,last_name,email,category) VALUES($1,$2,$3,$4,$5)",
            c.id, c.first_name, c.last_name, c.email, c.category);
        insert_phones(tx, c.id, c.phones);
        tx.commit();
    }

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

    bool remove(const std::string& id) {
        auto g = pool_.acquire();
        pqxx::work tx(g.get());
        auto r = tx.exec_params("DELETE FROM contacts WHERE id=$1", id);
        tx.commit();
        return r.affected_rows() > 0;
    }

    // ── Fuzzy dedup ───────────────────────────────────────────────────────
    static int levenshtein(const std::string& a, const std::string& b) {
        size_t m = a.size(), n = b.size();
        std::vector<std::vector<int>> dp(m+1, std::vector<int>(n+1));
        for (size_t i = 0; i <= m; ++i) dp[i][0] = i;
        for (size_t j = 0; j <= n; ++j) dp[0][j] = j;
        for (size_t i = 1; i <= m; ++i)
            for (size_t j = 1; j <= n; ++j)
                dp[i][j] = (a[i-1] == b[j-1]) ? dp[i-1][j-1]
                          : 1 + std::min({dp[i-1][j], dp[i][j-1], dp[i-1][j-1]});
        return dp[m][n];
    }

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
                        {"similarity", std::round(sim * 1000) / 1000},
                    });
            }
        }
        std::ranges::sort(pairs, [](const json& a, const json& b) {
            return a["similarity"].get<double>() > b["similarity"].get<double>();
        });
        return {{"pairs", json(pairs)}, {"total", pairs.size()}, {"threshold", threshold}};
    }

    // ── Analytics ─────────────────────────────────────────────────────────
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

        // top domains sorted desc
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

    // ── vCard import ──────────────────────────────────────────────────────
    int import_vcard(const std::string& data) {
        int imported = 0;
        std::istringstream ss(data);
        std::string line;
        Contact current;
        bool in_card = false;

        auto trim = [](std::string s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char c){ return !std::isspace(c); }));
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char c){ return !std::isspace(c); }).base(), s.end());
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
static std::string env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v ? v : fallback;
}

static void json_response(httplib::Response& res, int status, const json& body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

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

    std::atomic<uint64_t> req_count{0}, err_count{0};
    const auto start_time = std::chrono::steady_clock::now();

    std::thread([gateway_url] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        register_with_gateway(gateway_url);
    }).detach();

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(8); };

    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, {{"status","ok"},{"service","contacts-cpp"},{"threads",8}});
    });

    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
        json_response(res, 200, {{"requests",req_count.load()},{"errors",err_count.load()},{"uptime_s",uptime}});
    });

    svr.Get("/contacts", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        std::string q = req.has_param("q") ? req.get_param_value("q") : "";
        auto contacts = store.get_all(q);
        json arr = json::array();
        for (const auto& c : contacts) arr.push_back(contact_to_json(c));
        json_response(res, 200, arr);
    });

    svr.Get(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        auto opt = store.get_by_id(req.matches[1]);
        if (!opt) { ++err_count; json_response(res, 404, {{"error","not found"}}); return; }
        json_response(res, 200, contact_to_json(*opt));
    });

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

    svr.Delete(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        if (!store.remove(req.matches[1])) {
            ++err_count;
            json_response(res, 404, {{"error","not found"}});
            return;
        }
        json_response(res, 204, json(nullptr));
    });

    svr.Get("/db/tables", [&](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, store.raw_tables());
    });

    svr.Get("/dedup", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        double threshold = 0.85;
        if (req.has_param("threshold")) {
            try { threshold = std::stod(req.get_param_value("threshold")); }
            catch (...) {}
        }
        threshold = std::clamp(threshold, 0.0, 1.0);
        json_response(res, 200, store.dedup(threshold));
    });

    svr.Get("/analytics", [&](const httplib::Request&, httplib::Response& res) {
        ++req_count;
        json_response(res, 200, store.analytics());
    });

    svr.Get("/export/vcard", [&](const httplib::Request&, httplib::Response& res) {
        ++req_count;
        res.status = 200;
        res.set_header("Content-Disposition", "attachment; filename=\"contacts.vcf\"");
        res.set_content(store.export_vcard(), "text/vcard; charset=utf-8");
    });

    svr.Post("/import/vcard", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        int n = store.import_vcard(req.body);
        json_response(res, 200, {{"imported", n}});
    });

    std::cout << "[contacts-cpp] listening on 0.0.0.0:8080\n";
    svr.listen("0.0.0.0", 8080);
    return 0;
}
