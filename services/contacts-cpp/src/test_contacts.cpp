// test_contacts.cpp — minimální test framework pro ContactStore
// Kompiluje bez externích závislostí (pouze nlohmann/json a std C++20).

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;

// ===========================================================================
// Kopie produkčního kódu (bez httplib závislostí)
// ===========================================================================

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

    std::string uuid;
    uuid.reserve(36);
    uuid += hex4(8);
    uuid += '-';
    uuid += hex4(4);
    uuid += '-';
    uint32_t v = (dist(gen) & 0x0FFF) | 0x4000;
    std::string vs;
    vs.reserve(4);
    for (int i = 3; i >= 0; --i) {
        int n = (v >> (i * 4)) & 0xF;
        vs += (n < 10) ? char('0' + n) : char('a' + n - 10);
    }
    uuid += vs;
    uuid += '-';
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
        for (const auto& p : j["phones"])
            c.phones.push_back(phone_from_json(p));
    }
    return c;
}

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
        for (const auto& c : contacts_)
            if (c.id == id) return c;
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

    std::size_t size() const {
        std::shared_lock lk(mtx_);
        return contacts_.size();
    }

private:
    mutable std::shared_mutex mtx_;
    std::vector<Contact>      contacts_;
};

// ===========================================================================
// Minimální test framework
// ===========================================================================

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr) \
    do { \
        ++tests_run; \
        if (expr) { \
            ++tests_passed; \
        } else { \
            ++tests_failed; \
            std::cerr << std::format("  FAIL  {}:{} — {}\n", __FILE__, __LINE__, #expr); \
        } \
    } while (false)

#define CHECK_EQ(a, b) \
    do { \
        ++tests_run; \
        if ((a) == (b)) { \
            ++tests_passed; \
        } else { \
            ++tests_failed; \
            std::cerr << std::format("  FAIL  {}:{} — {} == {} (got {} vs {})\n", \
                __FILE__, __LINE__, #a, #b, (a), (b)); \
        } \
    } while (false)

static void run_test(const char* name, void(*fn)()) {
    std::cout << std::format("[ RUN ] {}\n", name);
    fn();
    std::cout << std::format("[  OK ] {}\n", name);
}

// ===========================================================================
// Testy
// ===========================================================================

static void test_generate_uuid_length() {
    auto uuid = generate_uuid();
    CHECK_EQ(uuid.size(), std::size_t(36));
}

static void test_generate_uuid_dashes() {
    auto uuid = generate_uuid();
    // xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx
    CHECK_EQ(uuid[8],  '-');
    CHECK_EQ(uuid[13], '-');
    CHECK_EQ(uuid[18], '-');
    CHECK_EQ(uuid[23], '-');
}

static void test_generate_uuid_version4() {
    auto uuid = generate_uuid();
    // pozice 14 musí být '4' (version 4)
    CHECK_EQ(uuid[14], '4');
}

static void test_generate_uuid_variant() {
    auto uuid = generate_uuid();
    // pozice 19: variant bits — první hex číslice musí být 8, 9, a nebo b
    char v = uuid[19];
    CHECK(v == '8' || v == '9' || v == 'a' || v == 'b');
}

static void test_generate_uuid_unique() {
    auto a = generate_uuid();
    auto b = generate_uuid();
    CHECK(a != b);
}

static void test_contact_from_json_defaults() {
    json j = {{"first_name", "Jan"}, {"last_name", "Novák"}};
    Contact c = contact_from_json(j, "test-id");
    CHECK_EQ(c.id,         std::string("test-id"));
    CHECK_EQ(c.first_name, std::string("Jan"));
    CHECK_EQ(c.last_name,  std::string("Novák"));
    CHECK_EQ(c.email,      std::string(""));
    CHECK_EQ(c.category,   std::string("Other"));
    CHECK(c.phones.empty());
}

static void test_contact_roundtrip() {
    json orig = {
        {"id",         "abc-123"},
        {"first_name", "Marie"},
        {"last_name",  "Horáková"},
        {"email",      "marie@example.com"},
        {"phones",     json::array({json{{"label","mobil"},{"number","+420 777 111 222"}}})},
        {"category",   "Family"},
    };
    Contact c  = contact_from_json(orig, "abc-123");
    json   out = contact_to_json(c);

    CHECK_EQ(out["id"].get<std::string>(),         std::string("abc-123"));
    CHECK_EQ(out["first_name"].get<std::string>(), std::string("Marie"));
    CHECK_EQ(out["last_name"].get<std::string>(),  std::string("Horáková"));
    CHECK_EQ(out["email"].get<std::string>(),      std::string("marie@example.com"));
    CHECK_EQ(out["category"].get<std::string>(),   std::string("Family"));
    CHECK_EQ(out["phones"].size(), std::size_t(1));
    CHECK_EQ(out["phones"][0]["label"].get<std::string>(),  std::string("mobil"));
    CHECK_EQ(out["phones"][0]["number"].get<std::string>(), std::string("+420 777 111 222"));
}

static void test_store_add_get_all() {
    ContactStore store;
    Contact c;
    c.id         = "id-1";
    c.first_name = "Jana";
    c.last_name  = "Nováková";
    c.email      = "jana@x.cz";
    c.category   = "Friend";
    store.add(c);

    auto all = store.get_all();
    CHECK_EQ(all.size(), std::size_t(1));
    CHECK_EQ(all[0].id,         std::string("id-1"));
    CHECK_EQ(all[0].first_name, std::string("Jana"));
}

static void test_store_get_all_sorted() {
    ContactStore store;
    for (auto&& [fn, ln] : std::vector<std::pair<std::string,std::string>>{
            {"Petr",  "Žák"},
            {"Anna",  "Adam"},
            {"Karel", "Beneš"}}) {
        Contact c;
        c.id = generate_uuid(); c.first_name = fn; c.last_name = ln;
        c.category = "Other";
        store.add(c);
    }
    auto all = store.get_all();
    CHECK_EQ(all.size(), std::size_t(3));
    // Řazení: Adam, Beneš, Žák
    CHECK_EQ(all[0].last_name, std::string("Adam"));
    CHECK_EQ(all[1].last_name, std::string("Beneš"));
    CHECK_EQ(all[2].last_name, std::string("Žák"));
}

static void test_store_get_all_query_match() {
    ContactStore store;
    for (auto&& [fn, ln, cat] : std::vector<std::tuple<std::string,std::string,std::string>>{
            {"Jana",  "Nováková", "Friend"},
            {"Petr",  "Svoboda",  "Colleague"},
            {"Marie", "Horáková", "Family"}}) {
        Contact c;
        c.id = generate_uuid(); c.first_name = fn; c.last_name = ln; c.category = cat;
        store.add(c);
    }
    // query podle příjmení
    auto res = store.get_all("Svoboda");
    CHECK_EQ(res.size(), std::size_t(1));
    CHECK_EQ(res[0].first_name, std::string("Petr"));
}

static void test_store_get_all_query_case_insensitive() {
    ContactStore store;
    Contact c; c.id = "x"; c.first_name = "Jana"; c.last_name = "Nováková";
    c.category = "Friend";
    store.add(c);
    auto res = store.get_all("JANA");
    CHECK_EQ(res.size(), std::size_t(1));
    auto empty = store.get_all("xyz_never_matches");
    CHECK(empty.empty());
}

static void test_store_get_by_id_found() {
    ContactStore store;
    Contact c; c.id = "known-id"; c.first_name = "Test"; c.last_name = "User";
    c.category = "Other";
    store.add(c);
    auto opt = store.get_by_id("known-id");
    CHECK(opt.has_value());
    CHECK_EQ(opt->first_name, std::string("Test"));
}

static void test_store_get_by_id_not_found() {
    ContactStore store;
    auto opt = store.get_by_id("does-not-exist");
    CHECK(!opt.has_value());
}

static void test_store_update_existing() {
    ContactStore store;
    Contact c; c.id = "u-1"; c.first_name = "Starý"; c.last_name = "Kontakt";
    c.category = "Other";
    store.add(c);

    Contact updated; updated.first_name = "Nový"; updated.last_name = "Kontakt";
    updated.email = "novy@x.cz"; updated.category = "Friend";
    bool ok = store.update("u-1", updated);
    CHECK(ok);

    auto opt = store.get_by_id("u-1");
    CHECK(opt.has_value());
    CHECK_EQ(opt->first_name, std::string("Nový"));
    CHECK_EQ(opt->email,      std::string("novy@x.cz"));
    CHECK_EQ(opt->id,         std::string("u-1"));  // id zachováno
}

static void test_store_update_not_found() {
    ContactStore store;
    Contact c; c.first_name = "X"; c.last_name = "Y"; c.category = "Other";
    bool ok = store.update("no-such-id", c);
    CHECK(!ok);
}

static void test_store_remove_existing() {
    ContactStore store;
    Contact c; c.id = "del-1"; c.first_name = "Smazat"; c.last_name = "Mě";
    c.category = "Other";
    store.add(c);
    bool ok = store.remove("del-1");
    CHECK(ok);
    CHECK(store.get_all().empty());
}

static void test_store_remove_not_found() {
    ContactStore store;
    bool ok = store.remove("ghost-id");
    CHECK(!ok);
}

static void test_store_thread_safety() {
    ContactStore store;
    constexpr int num_threads  = 10;
    constexpr int per_thread   = 100;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&store, t]() {
            for (int i = 0; i < per_thread; ++i) {
                Contact c;
                c.id         = std::format("t{}-c{}", t, i);
                c.first_name = std::format("Vlákno{}", t);
                c.last_name  = std::format("Kontakt{}", i);
                c.category   = "Other";
                store.add(c);
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK_EQ(store.size(), std::size_t(num_threads * per_thread));
}

// ===========================================================================
// main
// ===========================================================================

int main() {
    std::cout << "=== contacts-cpp unit tests ===\n\n";

    run_test("generate_uuid: délka 36",          test_generate_uuid_length);
    run_test("generate_uuid: pomlčky na pozicích 8,13,18,23", test_generate_uuid_dashes);
    run_test("generate_uuid: verze 4 (pozice 14='4')",        test_generate_uuid_version4);
    run_test("generate_uuid: variant bits (pozice 19)",        test_generate_uuid_variant);
    run_test("generate_uuid: unikátnost",                      test_generate_uuid_unique);

    run_test("contact_from_json: výchozí hodnoty",   test_contact_from_json_defaults);
    run_test("contact from/to JSON round-trip",      test_contact_roundtrip);

    run_test("ContactStore: add + get_all",           test_store_add_get_all);
    run_test("ContactStore: get_all řazení",          test_store_get_all_sorted);
    run_test("ContactStore: get_all s query filtrem", test_store_get_all_query_match);
    run_test("ContactStore: get_all case-insensitive query", test_store_get_all_query_case_insensitive);
    run_test("ContactStore: get_by_id nalezeno",      test_store_get_by_id_found);
    run_test("ContactStore: get_by_id nenalezeno",    test_store_get_by_id_not_found);
    run_test("ContactStore: update existující",       test_store_update_existing);
    run_test("ContactStore: update neexistující",     test_store_update_not_found);
    run_test("ContactStore: remove existující",       test_store_remove_existing);
    run_test("ContactStore: remove neexistující",     test_store_remove_not_found);
    run_test("ContactStore: thread safety (10 vláken x 100)", test_store_thread_safety);

    std::cout << std::format("\n=== Výsledek: {}/{} testů prošlo", tests_passed, tests_run);
    if (tests_failed > 0)
        std::cout << std::format(", {} selhalo", tests_failed);
    std::cout << " ===\n";

    return tests_failed == 0 ? 0 : 1;
}
