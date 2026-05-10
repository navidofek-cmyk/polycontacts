#include "contact_book.hpp"
#include <iostream>
#include <format>
#include <memory>

// ukázka konceptu Named — funguje s čímkoliv co má .name()
template <Named T>
void greet(const T& entity) {
    std::cout << std::format("Načteno: {}\n", entity.name());
}

int main() {
    // unique_ptr — jednoznačné vlastnictví, automatické uvolnění
    auto book = std::make_unique<ContactBook>();

    // designated initializers (C++20) — jasné pojmenování polí
    book->add(Contact{
        .first_name = "Jana",
        .last_name  = "Nováková",
        .email      = "jana@example.com",
        .phones     = {{"mobil", "+420 601 111 222"}},
        .category   = Category::Friend,
    });

    book->add(Contact{
        .first_name = "Petr",
        .last_name  = "Svoboda",
        .email      = std::nullopt,   // email nemá — explicitně prázdný
        .phones     = {{"práce", "+420 602 333 444"}, {"mobil", "+420 777 555 666"}},
        .category   = Category::Work,
    });

    book->add(Contact{
        .first_name = "Marie",
        .last_name  = "Svoboda",
        .email      = "marie@example.com",
        .phones     = {{"domů", "+420 603 777 888"}},
        .category   = Category::Family,
    });

    book->add(Contact{
        .first_name = "Tomáš",
        .last_name  = "Dvořák",
        .email      = "tomas@work.cz",
        .phones     = {{"mobil", "+420 604 999 000"}},
        .category   = Category::Work,
    });

    // ── ukázka konceptu ───────────────────────────────────────────────────────
    std::cout << "\n=== Koncept Named ===\n";
    for (const auto& c : book->all())
        greet(c);

    // ── seřazený výpis ────────────────────────────────────────────────────────
    std::cout << "\n=== Všechny kontakty (seřazeno) ===\n";
    for (const auto& c : book->sorted())
        print_contact(c);

    // ── vyhledávání ───────────────────────────────────────────────────────────
    std::cout << "\n=== Hledám 'svoboda' ===\n";
    auto found = book->search("svoboda");
    print_contacts(found);

    // ── filtrování přes ranges ────────────────────────────────────────────────
    std::cout << "\n=== Pouze pracovní kontakty ===\n";
    auto work = book->find_if([](const Contact& c) {
        return c.category == Category::Work;
    });
    print_contacts(work);

    // ── optional — bezpečné čtení emailu ─────────────────────────────────────
    std::cout << "\n=== Kontakty bez emailu ===\n";
    auto no_email = book->find_if([](const Contact& c) {
        return !c.email.has_value();
    });
    print_contacts(no_email);

    // ── odstranění ────────────────────────────────────────────────────────────
    std::cout << std::format("\n=== Odstraňuji 'Tomáš Dvořák' ===\n");
    auto removed = book->remove("Tomáš Dvořák");
    std::cout << std::format("Odstraněno: {} kontakt(ů)\n", removed);
    std::cout << std::format("Celkem kontaktů: {}\n", book->size());

    // unique_ptr se uvolní automaticky na konci scopu — žádný delete
    return 0;
}
