# RE Specifikace

Tento dokument popisuje systém tak, aby ho bylo možné napsat od nuly bez přístupu ke zdrojovému kódu. Jde o referenci pro pochopení toho, co každá komponenta musí dělat — nikoli jak to dělá.

## Datový model

```json
{
  "id":         "550e8400-e29b-41d4-a716-446655440000",  // UUID v4, generuje server
  "first_name": "Jana",                                   // required
  "last_name":  "Nováková",                               // required
  "email":      "jana@example.com",                       // optional, může být ""
  "phones": [                                             // optional, může být []
    { "label": "mobil", "number": "+420 601 111 222" }
  ],
  "category":   "Friend"                                  // výchozí "Other"
}
```

Povolené hodnoty `category`: `Friend`, `Work`, `Family`, `Other`, `Colleague`.

## Invarianty

- `id` je UUID verze 4 ve standardním formátu (`xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx`); generuje ho contacts-cpp, klient ho nikdy nepředepisuje
- `first_name` a `last_name` jsou povinné — POST i PUT vrátí 400 pokud chybí nebo jsou prázdné
- `phones` je pole objektů `{label, number}`; pole smí být prázdné, ale nesmí chybět klíč v odpovědi
- `category` musí být jedna z povolených hodnot; contacts-cpp ukládá libovolný řetězec, validaci provádí frontend
- Seřazení výstupu `GET /contacts` je vždy `(last_name ASC, first_name ASC)`
- Telefonní čísla se při `DELETE` kontaktu odstraní kaskádově (`ON DELETE CASCADE`)
- Seed data (4 kontakty) se vloží automaticky pokud je tabulka `contacts` prázdná při startu

## Gotchas

- **`SELF_URL` musí být Docker hostname**, ne `localhost`. Search-rust posílá svou adresu do gateway při registraci. Pokud `SELF_URL=http://localhost:8081`, gateway zaznamená tuto adresu a bff-python pak volá localhost — což uvnitř jiného kontejneru nefunguje. Správná hodnota je `http://search-rust:8081`.
- **Gateway není na datové cestě** pro čtení a zápis kontaktů. BFF volá contacts-cpp a search-rust přímo. Gateway je jen registry — nevidí obsah kontaktů, nezná je, nepřeposílá je.
- **Search index není perzistentní**. Při restartu search-rust kontejneru je index prázdný; search-rust ho při startu znovu načte z contacts-cpp. Pokud search-rust nastartuje dříve než contacts-cpp odpoví na `/contacts`, retry loop počká a zkusí to znovu.
- **`docker compose down` bez `-v` zachová data** — PostgreSQL volume `postgres_data` přežije. Pouze `docker compose down -v` smaže volume a s ním i všechny kontakty.
- **Notifikace search je best-effort**. Contacts-cpp po POST/PUT spustí detached thread, který POSTuje na `search-rust/index`. Pokud search-rust není dostupný, chyba se ignoruje — odpověď klientovi odejde bez čekání.

## Soubory projektu

```
polycontacts/
├── services/
│   ├── docker-compose.yml
│   ├── contacts-cpp/
│   │   ├── CMakeLists.txt
│   │   ├── Dockerfile
│   │   └── src/
│   │       └── main.cpp          ← celá služba v jednom souboru
│   ├── search-rust/
│   │   ├── Cargo.toml
│   │   ├── Dockerfile
│   │   └── src/
│   │       └── main.rs           ← celá služba v jednom souboru
│   ├── gateway-go/
│   │   ├── go.mod
│   │   ├── Dockerfile
│   │   ├── main.go               ← celá služba v jednom souboru
│   │   └── main_test.go
│   └── bff-python/
│       ├── pyproject.toml
│       ├── Dockerfile
│       ├── app/
│       │   ├── main.py           ← celá služba v jednom souboru
│       │   └── static/
│       │       └── index.html    ← SPA frontend
│       └── tests/
│           └── test_main.py
├── docs/                         ← tento tutorial (MkDocs)
├── mkdocs.yml
├── ARCHITECTURE.md               ← původní RE spec
└── README.md
```
