# search-rust — Invertovaný index

## Proč Rust

- **Žádný GC** — vyhledávání nikdy nezastavuje garbage collector. Latence je proto konzistentní: p50 i p99.9 se liší jen 2.5×, zatímco u JVM nebo Python procesů mohou GC pauzy způsobit výkyvy o řád výše.
- **`tokio::sync::RwLock`** — asynchronní čtecí/zápisový zámek. Stovky souběžných vyhledávání probíhají paralelně bez blokování OS vláken; indexování (zápis) čeká jen na dokončení čtení.
- **`async` bez blokování** — `tokio::spawn` spustí startup sekvenci jako lightweight task. Server začne přijímat zdravotní kontroly okamžitě, index se plní na pozadí.

## Invertovaný index

Datová struktura indexu:

```
inverted_index: HashMap<String, Vec<(contact_id, váha)>>

Příklad:
  "novak"    → [("id-42", 3.0), ("id-7", 1.0)]
  "jana"     → [("id-42", 2.0)]
  "example"  → [("id-42", 1.0), ("id-7", 1.0)]
```

Při vyhledávání stačí najít token v `HashMap` — O(1) — a sečíst váhy. Celý index žije v RAM, pro tisíce kontaktů je to jednotky MB.

Tokenizace rozloží vstup na malá písmena a rozdělí na hranicích nealfanumerických znaků. E-mail `jana.novakova@example.com` dá tokeny `["jana", "novakova", "example", "com"]`.

```rust
fn tokenize(s: &str) -> Vec<String> {
    s.to_lowercase()
        .split(|c: char| !c.is_alphanumeric())
        .filter(|t| !t.is_empty())
        .map(String::from)
        .collect()
}
```

## Váhy vyhledávání

| Pole | Váha | Proč |
|---|---|---|
| `last_name` | 3.0 | Uživatelé hledají primárně podle příjmení |
| `first_name` | 2.0 | Jméno je druhý nejčastější způsob hledání |
| `email` | 1.0 | Doplňková informace, ne primární identifikátor |
| `phones` | 1.0 | Občas užitečné hledat dle čísla nebo štítku |
| `category` | 1.0 | Filtrace dle kategorie má stejnou prioritu jako email |

Váhy jsou kumulativní: pokud slovo „novakova" je příjmení i část e-mailu téhož kontaktu, dostane skóre 3.0 + 1.0 = 4.0.

## Exact vs prefix match

Algoritmus kombinuje dva typy shody pro každý token v dotazu:

**Exact match** — token v dotazu se přesně shoduje s tokenem v indexu. Skóre = plná váha. Příklad: dotaz `jana` najde přesně token `jana`.

**Prefix match** — token v indexu *začíná* dotazovým tokenem. Skóre = váha × 0.5 (penalizace za neúplnost). Příklad: dotaz `Nov` najde tokeny `nováková`, `novotný` atd. Díky tomu funguje vyhledávání během psaní — výsledky se aktualizují po každém napsaném písmenu.

Penalizace 0.5 je záměrná: exact match musí vždy předčit prefix match se stejným základním skóre, aby výsledky byly intuitivní.

## Výkon

Naměřeno load testem: 1000 požadavků, 20 souběžných spojení.

| Metrika | Hodnota |
|---|---|
| req/s | 4 193 |
| p50 | 4.3 ms |
| p99 | 8.9 ms |
| p99.9 | 11.3 ms |

Konzistentnost (p99.9 je jen 2.6× p50) je přímým důsledkem absence GC. Porovnej s contacts-cpp, kde p99 je 133× p50 kvůli lock contention.
