#include "contact_book.hpp"
#include <algorithm>
#include <ranges>
#include <format>
#include <iostream>
#include <cctype>

// ── ContactBook ───────────────────────────────────────────────────────────────

Contact& ContactBook::add(Contact contact) {
    return contacts_.emplace_back(std::move(contact));
}

std::vector<const Contact*> ContactBook::find_if(std::function<bool(const Contact&)> pred) const {
    // std::ranges::filter_view — líné vyhodnocení, žádné zbytečné kopie
    auto view = contacts_ | std::views::filter(pred);

    std::vector<const Contact*> result;
    for (const auto& c : view)
        result.push_back(&c);
    return result;
}

std::vector<const Contact*> ContactBook::search(std::string_view query) const {
    return find_if([&](const Contact& c) {
        return icontains(c.first_name, query) ||
               icontains(c.last_name,  query) ||
               (c.email && icontains(*c.email, query));
    });
}

std::vector<Contact> ContactBook::sorted() const {
    auto copy = contacts_;
    std::ranges::sort(copy, [](const Contact& a, const Contact& b) {
        if (a.last_name != b.last_name) return a.last_name < b.last_name;
        return a.first_name < b.first_name;
    });
    return copy;
}

std::size_t ContactBook::remove(std::string_view full_name) {
    auto [first, last] = std::ranges::remove_if(contacts_, [&](const Contact& c) {
        return c.name() == full_name;
    });
    std::size_t n = std::distance(first, last);
    contacts_.erase(first, last);
    return n;
}

bool ContactBook::icontains(std::string_view haystack, std::string_view needle) {
    auto to_lower = [](std::string_view s) {
        std::string out(s);
        std::ranges::transform(out, out.begin(), [](unsigned char c) { return std::tolower(c); });
        return out;
    };
    return to_lower(haystack).find(to_lower(needle)) != std::string::npos;
}

// ── výpis ─────────────────────────────────────────────────────────────────────

std::string category_name(Category c) {
    // switch přes enum class — kompilátor upozorní na chybějící větev
    switch (c) {
        case Category::Friend: return "přátelé";
        case Category::Work:   return "práce";
        case Category::Family: return "rodina";
        case Category::Other:  return "ostatní";
    }
    return "?";
}

void print_contact(const Contact& c) {
    // std::format — typově bezpečná náhrada printf (C++20)
    std::cout << std::format("  {:.<30} [{}]\n", c.name(), category_name(c.category));

    if (c.email)
        std::cout << std::format("    email:  {}\n", *c.email);

    for (const auto& [label, number] : c.phones)   // structured binding
        std::cout << std::format("    {:<8} {}\n", label + ":", number);
}

void print_contacts(std::span<const Contact* const> contacts) {
    if (contacts.empty()) {
        std::cout << "  (žádné výsledky)\n";
        return;
    }
    for (const auto* c : contacts)
        print_contact(*c);
}
