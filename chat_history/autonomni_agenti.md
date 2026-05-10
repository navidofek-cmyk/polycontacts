# Autonomní agenti — návod pro polycontacts

## Proč agenti selhávají

Agent selže nebo se zastaví a čeká když:
1. Neví jak projekt spustit / testovat
2. Narazí na chybu a neví co dělat
3. Chybí mu kontext o architektuře
4. Prompt je příliš vágní

## Anatomie dobrého autonomního promptu

```
[CO] Napiš pytest testy pro FastAPI BFF v /home/ivand/projects/contacts/services/bff-python/app/main.py

[KONTEXT] Přečti soubor. Přečti také CLAUDE.md v kořeni projektu.

[JAK] Mockuj httpx.AsyncClient přes vlastní _AsyncMockTransport — 
      ne přes patch('app.main._client'), to nefunguje před lifespan.
      Testuj: /health, /api/contacts (GET+POST), /api/search, /api/stats, /api/services.

[SPUSŤ] cd services/bff-python && uv run pytest tests/ -v

[OPRAV] Oprav všechny chyby dokud testy neprojdou. Nezastavuj se na první chybě.
```

Klíčové části:
- **CO** — přesný soubor, ne "napiš testy"
- **KONTEXT** — kde hledat informace
- **JAK** — gotchas a neobvyklé věci předem
- **SPUSŤ** — přesný příkaz
- **OPRAV** — explicit povolení iterovat

## Paralelní agenti

Claude Code může spustit N agentů najednou. Jsou efektivní když:
- Úkoly jsou **nezávislé** (různé soubory/služby)
- Každý má **vlastní kontext** (nepředpokládej sdílenou paměť)
- Výsledky se **agregují** na konci

Příklad — testy pro 4 služby najednou:
```
Spusť 4 agenty paralelně:

Agent 1 — pytest pro bff-python:
  Soubor: services/bff-python/app/main.py + tests/test_main.py
  Spusť: cd services/bff-python && uv run pytest tests/ -v
  Oprav chyby dokud neprojdou.

Agent 2 — go test pro gateway-go:
  Soubor: services/gateway-go/main.go + main_test.go  
  Spusť: docker run --rm -v $(pwd)/services/gateway-go:/src -w /src golang:1.22 go test -v ./...
  Oprav chyby dokud neprojdou.

Agent 3 — cargo test pro search-rust:
  Soubor: services/search-rust/src/main.rs
  Spusť: cd services/search-rust && cargo test
  Oprav chyby.

Agent 4 — C++ test runner:
  Soubory: services/contacts-cpp/src/*.cpp + CMakeLists.txt
  Spusť: cd services/contacts-cpp && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel $(nproc) && ./build/test_contacts
  Oprav chyby.
```

## Nastavení Claude Code pro maximální autonomii

### settings.local.json (projektová)
```json
{
  "permissions": {
    "defaultMode": "bypassPermissions"
  },
  "hooks": {
    "PostToolUse": [{
      "matcher": "Edit|Write",
      "hooks": [{
        "type": "command",
        "command": "...",  // syntax check po každém editaci
        "timeout": 10
      }]
    }],
    "Stop": [{
      "matcher": "",
      "hooks": [{
        "type": "command", 
        "command": "...",  // health check kontejnerů na konci
        "timeout": 8
      }]
    }]
  }
}
```

### ~/.claude/settings.json (globální)
```json
{
  "model": "sonnet",
  "skipDangerousModePermissionPrompt": true
}
```

### CLAUDE.md (projektový kontext)
Klíčový soubor — Claude ho načte automaticky při každém chatu.
Musí obsahovat:
- Co projekt dělá (2 věty)
- Jak spustit a testovat (přesné příkazy)
- Kde jsou klíčové soubory
- Pravidla (co nedělat, co vždy dělat)

## Hooks — automatické akce

| Hook | Kdy se spustí | Příklad použití |
|---|---|---|
| `PostToolUse` + `Edit\|Write` | Po každé editaci souboru | Syntax check, lint, format |
| `PostToolUse` + `Bash` | Po každém bash příkazu | Logování, monitoring |
| `Stop` | Když Claude skončí | Health check, notifikace |
| `PreCompact` | Před kompakcí kontextu | Uložit důležité info |

## Anti-patterny

❌ `"napiš testy pro projekt"` — Claude neví pro co, v jakém jazyce, jak spustit

❌ `"oprav bugy"` — které bugy? kde?

❌ `"udělej to jako minule"` — agent nemá paměť předchozích sessí

✅ `"napiš unit testy pro ContactStore v services/contacts-cpp/src/main.cpp, použij vlastní assert makra bez externích deps, spusť ./build/test_contacts a oprav vše dokud neprojdou"`

## Workflow pro velké úkoly

1. **CLAUDE.md** — jednou napsat, platí navždy
2. **bypassPermissions** — žádné přerušení pro povolení
3. **Hooks** — automatická kontrola po každé akci
4. **Paralelní agenti** — nezávislé části najednou
5. **Přesné prompty** — soubor + příkaz + "oprav dokud neprojde"
