#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <optional>
#include <random>
#include <ranges>
#include <shared_mutex>
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

    auto hex4 = [&](int nibbles) -> std::string {
        uint32_t val = dist(gen);
        std::string s;
        s.reserve(nibbles);
        for (int i = nibbles - 1; i >= 0; --i) {
            int nibble = (val >> (i * 4)) & 0xF;
            s += (nibble < 10) ? char('0' + nibble) : char('a' + nibble - 10);
        }
        return s;
    };

    // xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    std::string uuid;
    uuid.reserve(36);
    uuid += hex4(8);
    uuid += '-';
    uuid += hex4(4);
    uuid += '-';
    // version 4
    uint32_t v = (dist(gen) & 0x0FFF) | 0x4000;
    std::string vs;
    vs.reserve(4);
    for (int i = 3; i >= 0; --i) {
        int n = (v >> (i * 4)) & 0xF;
        vs += (n < 10) ? char('0' + n) : char('a' + n - 10);
    }
    uuid += vs;
    uuid += '-';
    // variant bits
    uint32_t var = (dist(gen) & 0x3FFF) | 0x8000;
    std::string vars;
    vars.reserve(4);
    for (int i = 3; i >= 0; --i) {
        int n = (var >> (i * 4)) & 0xF;
        vars += (n < 10) ? char('0' + n) : char('a' + n - 10);
    }
    uuid += vars;
    uuid += '-';
    uuid += hex4(8);
    uuid += hex4(4);
    return uuid;
}

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------
struct PhoneNumber {
    std::string label;
    std::string number;
};

struct Contact {
    std::string id;
    std::string first_name;
    std::string last_name;
    std::string email;
    std::vector<PhoneNumber> phones;
    std::string category;
};

// JSON serialization helpers
static json phone_to_json(const PhoneNumber& p) {
    return json{{"label", p.label}, {"number", p.number}};
}

static PhoneNumber phone_from_json(const json& j) {
    return PhoneNumber{
        .label  = j.value("label", ""),
        .number = j.value("number", ""),
    };
}

static json contact_to_json(const Contact& c) {
    json phones = json::array();
    for (const auto& p : c.phones) phones.push_back(phone_to_json(p));
    return json{
        {"id",         c.id},
        {"first_name", c.first_name},
        {"last_name",  c.last_name},
        {"email",      c.email},
        {"phones",     phones},
        {"category",   c.category},
    };
}

static Contact contact_from_json(const json& j, const std::string& id = "") {
    Contact c;
    c.id         = id.empty() ? j.value("id", generate_uuid()) : id;
    c.first_name = j.value("first_name", "");
    c.last_name  = j.value("last_name", "");
    c.email      = j.value("email", "");
    c.category   = j.value("category", "Other");
    if (j.contains("phones") && j["phones"].is_array()) {
        for (const auto& p : j["phones"]) {
            c.phones.push_back(phone_from_json(p));
        }
    }
    return c;
}

// ---------------------------------------------------------------------------
// ContactStore
// ---------------------------------------------------------------------------
class ContactStore {
public:
    void add(Contact c) {
        std::unique_lock lk(mtx_);
        contacts_.push_back(std::move(c));
    }

    std::vector<Contact> get_all(const std::string& q = "") const {
        std::shared_lock lk(mtx_);
        std::vector<Contact> result;
        result.reserve(contacts_.size());

        auto lower_q = q;
        std::transform(lower_q.begin(), lower_q.end(), lower_q.begin(), ::tolower);

        for (const auto& c : contacts_) {
            if (!q.empty()) {
                auto haystack = c.first_name + " " + c.last_name + " " +
                                c.email + " " + c.category;
                std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
                if (haystack.find(lower_q) == std::string::npos) continue;
            }
            result.push_back(c);
        }

        std::ranges::sort(result, [](const Contact& a, const Contact& b) {
            if (a.last_name != b.last_name) return a.last_name < b.last_name;
            return a.first_name < b.first_name;
        });
        return result;
    }

    std::optional<Contact> get_by_id(const std::string& id) const {
        std::shared_lock lk(mtx_);
        for (const auto& c : contacts_) {
            if (c.id == id) return c;
        }
        return std::nullopt;
    }

    bool update(const std::string& id, Contact updated) {
        std::unique_lock lk(mtx_);
        for (auto& c : contacts_) {
            if (c.id == id) {
                updated.id = id;
                c = std::move(updated);
                return true;
            }
        }
        return false;
    }

    bool remove(const std::string& id) {
        std::unique_lock lk(mtx_);
        auto it = std::ranges::find_if(contacts_,
            [&id](const Contact& c) { return c.id == id; });
        if (it == contacts_.end()) return false;
        contacts_.erase(it);
        return true;
    }

private:
    mutable std::shared_mutex mtx_;
    std::vector<Contact>      contacts_;
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

// Notify the search service to re-index a contact (best-effort, non-blocking)
static void notify_search(const std::string& search_url, const Contact& c) {
    std::thread([search_url, c]() {
        try {
            httplib::Client cli(search_url);
            cli.set_connection_timeout(2);
            cli.set_read_timeout(2);
            auto body = contact_to_json(c).dump();
            cli.Post("/index", body, "application/json");
        } catch (...) {}
    }).detach();
}

// ---------------------------------------------------------------------------
// Gateway registration
// ---------------------------------------------------------------------------
static void register_with_gateway(const std::string& gateway_url) {
    json reg = {
        {"name",        "contacts"},
        {"url",         "http://contacts-cpp:8080"},
        {"health_path", "/health"},
    };
    for (int attempt = 1; attempt <= 5; ++attempt) {
        try {
            httplib::Client cli(gateway_url);
            cli.set_connection_timeout(3);
            cli.set_read_timeout(3);
            auto res = cli.Post("/services", reg.dump(), "application/json");
            if (res && res->status < 300) {
                std::cout << std::format("[gateway] registered on attempt {}\n", attempt);
                return;
            }
            std::cout << std::format("[gateway] attempt {} failed (status {}), retrying...\n",
                                     attempt, res ? res->status : 0);
        } catch (const std::exception& e) {
            std::cout << std::format("[gateway] attempt {} exception: {}\n", attempt, e.what());
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    std::cerr << "[gateway] registration failed after 5 attempts\n";
}

// ---------------------------------------------------------------------------
// Seed data
// ---------------------------------------------------------------------------
static void seed_contacts(ContactStore& store) {
    store.add(contact_from_json(json{
        {"first_name", "Jana"},
        {"last_name",  "Nováková"},
        {"email",      "jana@example.com"},
        {"phones",     json::array({json{{"label","mobil"},{"number","+420 601 111 222"}}})},
        {"category",   "Friend"},
    }));
    store.add(contact_from_json(json{
        {"first_name", "Petr"},
        {"last_name",  "Svoboda"},
        {"email",      "petr.svoboda@work.cz"},
        {"phones",     json::array({json{{"label","work"},{"number","+420 602 333 444"}}})},
        {"category",   "Colleague"},
    }));
    store.add(contact_from_json(json{
        {"first_name", "Marie"},
        {"last_name",  "Horáková"},
        {"email",      "marie.horakova@home.cz"},
        {"phones",     json::array({
            json{{"label","home"},{"number","+420 603 555 666"}},
            json{{"label","mobil"},{"number","+420 777 888 999"}},
        })},
        {"category",   "Family"},
    }));
    store.add(contact_from_json(json{
        {"first_name", "Tomáš"},
        {"last_name",  "Dvořák"},
        {"email",      "tomas@example.com"},
        {"phones",     json::array()},
        {"category",   "Other"},
    }));
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main() {
    const std::string gateway_url = env_or("GATEWAY_URL", "http://localhost:9000");
    const std::string search_url  = env_or("SEARCH_URL",  "http://localhost:8081");

    ContactStore store;
    seed_contacts(store);

    std::atomic<uint64_t> req_count{0};
    std::atomic<uint64_t> err_count{0};
    const auto start_time = std::chrono::steady_clock::now();

    // Register with gateway in background so server can start first
    std::thread([gateway_url]() {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        register_with_gateway(gateway_url);
    }).detach();

    httplib::Server svr;
    svr.new_task_queue = [] { return new httplib::ThreadPool(8); };

    // ------------------------------------------------------------------
    // GET /health
    // ------------------------------------------------------------------
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json_response(res, 200, {
            {"status",  "ok"},
            {"service", "contacts-cpp"},
            {"threads", 8},
        });
    });

    // ------------------------------------------------------------------
    // GET /stats
    // ------------------------------------------------------------------
    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        auto now     = std::chrono::steady_clock::now();
        auto uptime  = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
        json_response(res, 200, {
            {"requests", req_count.load()},
            {"errors",   err_count.load()},
            {"uptime_s", uptime},
        });
    });

    // ------------------------------------------------------------------
    // GET /contacts
    // ------------------------------------------------------------------
    svr.Get("/contacts", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        std::string q = req.has_param("q") ? req.get_param_value("q") : "";
        auto contacts = store.get_all(q);
        json arr = json::array();
        for (const auto& c : contacts) arr.push_back(contact_to_json(c));
        json_response(res, 200, arr);
    });

    // ------------------------------------------------------------------
    // GET /contacts/:id
    // ------------------------------------------------------------------
    svr.Get(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        const std::string id = req.matches[1];
        auto opt = store.get_by_id(id);
        if (!opt) {
            ++err_count;
            json_response(res, 404, {{"error", std::format("contact {} not found", id)}});
            return;
        }
        json_response(res, 200, contact_to_json(*opt));
    });

    // ------------------------------------------------------------------
    // POST /contacts
    // ------------------------------------------------------------------
    svr.Post("/contacts", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        try {
            auto j = json::parse(req.body);
            Contact c = contact_from_json(j, generate_uuid());
            if (c.first_name.empty() || c.last_name.empty()) {
                ++err_count;
                json_response(res, 400, {{"error", "first_name and last_name are required"}});
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

    // ------------------------------------------------------------------
    // PUT /contacts/:id
    // ------------------------------------------------------------------
    svr.Put(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        const std::string id = req.matches[1];
        try {
            auto j = json::parse(req.body);
            Contact updated = contact_from_json(j, id);
            if (updated.first_name.empty() || updated.last_name.empty()) {
                ++err_count;
                json_response(res, 400, {{"error", "first_name and last_name are required"}});
                return;
            }
            if (!store.update(id, updated)) {
                ++err_count;
                json_response(res, 404, {{"error", std::format("contact {} not found", id)}});
                return;
            }
            notify_search(search_url, updated);
            json_response(res, 200, contact_to_json(updated));
        } catch (const json::exception& e) {
            ++err_count;
            json_response(res, 400, {{"error", std::string("invalid JSON: ") + e.what()}});
        }
    });

    // ------------------------------------------------------------------
    // DELETE /contacts/:id
    // ------------------------------------------------------------------
    svr.Delete(R"(/contacts/([^/]+))", [&](const httplib::Request& req, httplib::Response& res) {
        ++req_count;
        const std::string id = req.matches[1];
        if (!store.remove(id)) {
            ++err_count;
            json_response(res, 404, {{"error", std::format("contact {} not found", id)}});
            return;
        }
        json_response(res, 204, json(nullptr));
    });

    std::cout << std::format("[contacts-cpp] listening on 0.0.0.0:8080\n");
    svr.listen("0.0.0.0", 8080);
    return 0;
}
