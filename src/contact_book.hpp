#pragma once
#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <span>
#include <functional>

// ── Koncepty ─────────────────────────────────────────────────────────────────

// Cokoliv co má .name() vracející string lze použít jako pojmenovanou entitu
template <typename T>
concept Named = requires(T t) {
    { t.name() } -> std::convertible_to<std::string>;
};

// ── Datové typy ───────────────────────────────────────────────────────────────

enum class Category { Friend, Work, Family, Other };

struct PhoneNumber {
    std::string label;   // "mobil", "práce" ...
    std::string number;

    auto operator<=>(const PhoneNumber&) const = default;
};

struct Contact {
    std::string        first_name;
    std::string        last_name;
    std::optional<std::string>       email;     // nemusí být
    std::vector<PhoneNumber>         phones;
    Category                         category{Category::Other};

    // splňuje koncept Named
    std::string name() const { return first_name + " " + last_name; }

    // porovnání podle příjmení pak jména
    auto operator<=>(const Contact&) const = default;
};

// ── ContactBook ───────────────────────────────────────────────────────────────

class ContactBook {
public:
    // přidá kontakt, vrátí referenci na uložený
    Contact& add(Contact contact);

    // vyhledá podle predikátu — vrátí view (žádné kopírování)
    std::vector<const Contact*> find_if(std::function<bool(const Contact&)> pred) const;

    // vyhledá podle jména (case-insensitive, částečná shoda)
    std::vector<const Contact*> search(std::string_view query) const;

    // seřazená kopie (použije <=> z Contact)
    std::vector<Contact> sorted() const;

    // bezpečný přístup k prvku (std::span = pohled bez kopie)
    std::span<const Contact> all() const { return contacts_; }

    std::size_t size() const { return contacts_.size(); }

    // odstraní kontakt podle jména, vrátí počet odstraněných
    std::size_t remove(std::string_view full_name);

private:
    std::vector<Contact> contacts_;

    static bool icontains(std::string_view haystack, std::string_view needle);
};

// ── pomocné funkce ─────────────────────────────────────────────────────────

std::string category_name(Category c);
void        print_contact(const Contact& c);
void        print_contacts(std::span<const Contact* const> contacts);
