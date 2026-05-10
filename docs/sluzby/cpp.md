# contacts-cpp — C++20 CRUD

## Zodpovědnost

Contacts-cpp je autoritativní zdroj dat o kontaktech. Zodpovídá za veškerý přímý přístup k PostgreSQL — ostatní služby data o kontaktech čtou vždy přes HTTP API contacts-cpp, nikdy přímo z DB.

## C++20 features

| Feature | Kde se používá | Proč |
|---|---|---|
| `std::format` | Log výpisy, chybové zprávy | Typově bezpečná alternativa k `printf` — kompilátor ověří formátovací řetězec |
| `std::ranges::sort` | Seřazení deduplikačních párů, analytics | Čitelnější než `std::sort` s iterátory; range se předá přímo |
| `std::views::take(10)` | Top 10 domén v analytics | Lazy view — nekopíruje data, ořezává výstup za chodu |
| `std::optional<Contact>` | Návratová hodnota `get_by_id` | Explicitní vyjádření „kontakt nemusí existovat" bez výjimky |
| `std::atomic<uint64_t>` | Čítače požadavků a chyb | Lock-free inkrementace z více vláken HTTP serveru |
| Structured bindings | `for (auto& [k,v] : ...)` | Rozbalení páru bez `.first`/`.second` |

## Connection pool

Otevření nového PostgreSQL spojení trvá desítky milisekund — navazuje se TCP spojení, autentizace, negotiace protokolu. Pro 8 HTTP worker vláken by to znamenalo plýtvání i v klidu. `ConnPool` proto udržuje 8 připravených spojení a recykluje je.

Klíčová část je RAII guard — spojení se vrátí do poolu automaticky při zániku guardu, i v případě výjimky:

```cpp
struct Guard {
    ConnPool* pool;
    std::unique_ptr<pqxx::connection> conn;

    // Destruktor = automatické vrácení, i při výjimce v handleru
    ~Guard() { if (conn) pool->release(std::move(conn)); }

    pqxx::connection& get() { return *conn; }
};

Guard acquire() const {
    std::unique_lock lk(mtx_);
    // Zablokuje vlákno dokud se spojení neuvolní — žádný busy-loop
    cv_.wait(lk, [this] { return !pool_.empty(); });
    auto c = std::move(pool_.front());
    pool_.pop();
    return Guard{const_cast<ConnPool*>(this), std::move(c)};
}
```

`condition_variable::wait` je správný způsob čekání v C++ — vlákno skutečně spí a nepálí CPU.

## Levenshteinova deduplikace

Endpoint `/dedup` hledá kontakty s podobným jménem pomocí Levenshteinovy vzdálenosti — minimálního počtu jednoznakových úprav (vložení, smazání, nahrazení) pro převod jednoho jména na druhé. Výsledkem je skóre podobnosti v rozsahu 0–1, kde 1 znamená identická jména. Výchozí práh je 0.85.

```cpp
static int levenshtein(const std::string& a, const std::string& b) {
    size_t m = a.size(), n = b.size();
    std::vector<std::vector<int>> dp(m+1, std::vector<int>(n+1));
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

## API

| Metoda | Cesta | Popis | Status |
|---|---|---|---|
| `GET` | `/contacts?q=` | Seznam kontaktů, volitelně filtrovaný LIKE | 200 |
| `GET` | `/contacts/{id}` | Jeden kontakt | 200 / 404 |
| `POST` | `/contacts` | Vytvoří kontakt, vygeneruje UUID | 201 |
| `PUT` | `/contacts/{id}` | Přepíše kontakt | 200 / 404 |
| `DELETE` | `/contacts/{id}` | Smaže kontakt + telefony (CASCADE) | 204 / 404 |
| `GET` | `/dedup?threshold=` | Potenciální duplicity (výchozí 0.85) | 200 |
| `GET` | `/analytics` | Statistiky kategorií, domén, telefonů | 200 |
| `GET` | `/export/vcard` | Export jako `.vcf` soubor (vCard 3.0) | 200 |
| `POST` | `/import/vcard` | Import vCard souboru | 200 |
| `GET` | `/db/tables` | Surová data obou tabulek (debug) | 200 |
| `GET` | `/health` | `{"status":"ok","service":"contacts-cpp"}` | 200 |
| `GET` | `/stats` | Počty požadavků, chyb, uptime | 200 |
