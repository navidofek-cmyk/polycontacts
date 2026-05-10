"""
Backend-for-Frontend (BFF) pro aplikaci Contacts.

Tento modul slouží jako jediný vstupní bod pro prohlížeč: agreguje API
ze tří backendových služeb (contacts-cpp, search-rust, gateway-go)
a zároveň obsluhuje statické soubory frontendu. Klienti tak komunikují
pouze s jednou adresou a nemusí znát interní síť kontejnerů.
"""

from __future__ import annotations

import asyncio
import os
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from fastapi import FastAPI, Request, Response
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles

GATEWAY_URL = os.getenv("GATEWAY_URL", "http://localhost:9000")
CONTACTS_URL = os.getenv("CONTACTS_URL", "http://localhost:8080")
SEARCH_URL = os.getenv("SEARCH_URL", "http://localhost:8081")
PORT = int(os.getenv("PORT", "8989"))

STATIC_DIR = Path(__file__).parent / "static"

_client: httpx.AsyncClient


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Řídí životní cyklus sdíleného HTTP klienta a registraci u gateway.

    httpx.AsyncClient se vytváří jednou při startu — díky connection poolingu
    je opakované použití výrazně efektivnější než zakládání nového spojení
    pro každý požadavek.
    """
    global _client
    _client = httpx.AsyncClient(timeout=10.0)

    # Gateway udržuje live seznam zdravých služeb; registrace při startu zajistí,
    # že gateway ví o tomto BFF okamžitě po spuštění, aniž by čekala na svůj
    # vlastní discovery cyklus. Selhání ignorujeme — gateway nemusí být
    # v tuto chvíli ještě dostupná (závisí na pořadí startu kontejnerů).
    try:
        await _client.post(
            f"{GATEWAY_URL}/services",
            json={
                "name": "bff-python",
                "url": "http://bff-python:8989",
                "health_path": "/health",
            },
        )
    except Exception:
        pass  # gateway may not be up yet; ignore

    yield

    await _client.aclose()


app = FastAPI(title="bff-python", lifespan=lifespan)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

async def _proxy(request: Request, url: str, **kwargs) -> Response:
    """Přepošle příchozí HTTP požadavek na upstream službu a vrátí její odpověď.

    Přeposílá metodu, tělo a většinu hlaviček původního požadavku, takže
    upstream služba vidí kontext volání (např. Content-Type, Accept).
    """
    body = await request.body()
    upstream = await _client.request(
        method=request.method,
        url=url,
        content=body,
        headers={
            k: v
            for k, v in request.headers.items()
            # „host" by způsobilo odmítnutí požadavku upstreamem (neshoduje se
            # s jeho doménou). „content-length" přepočítá httpx automaticky
            # z předaného těla — ruční přeposlání by mohlo způsobit neshodu
            # délky při případném překódování.
            if k.lower() not in ("host", "content-length")
        },
        **kwargs,
    )
    return Response(
        content=upstream.content,
        status_code=upstream.status_code,
        media_type=upstream.headers.get("content-type", "application/json"),
    )


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

@app.get("/health")
async def health():
    """Jednoduchý liveness endpoint — gateway ho volá periodicky, aby věděla,
    zda je tato instance stále nahoře."""
    return {"status": "ok", "service": "bff-python"}


@app.get("/")
async def index():
    """Vrátí hlavní SPA stránku; veškerá interakce s API probíhá přes JS v ní."""
    return FileResponse(STATIC_DIR / "index.html")


# Contacts -------------------------------------------------------------------

@app.get("/api/contacts")
async def get_contacts(request: Request, q: str = ""):
    """Vrátí seznam kontaktů, volitelně filtrovaný fulltextovým dotazem q."""
    return await _proxy(request, f"{CONTACTS_URL}/contacts", params={"q": q})


@app.post("/api/contacts")
async def create_contact(request: Request):
    """Vytvoří nový kontakt; tělo požadavku předá beze změny do contacts-cpp."""
    return await _proxy(request, f"{CONTACTS_URL}/contacts")


@app.put("/api/contacts/{contact_id:path}")
async def update_contact(request: Request, contact_id: str):
    """Aktualizuje existující kontakt identifikovaný cestou (může obsahovat lomítka)."""
    return await _proxy(request, f"{CONTACTS_URL}/contacts/{contact_id}")


@app.delete("/api/contacts/{contact_id:path}")
async def delete_contact(request: Request, contact_id: str):
    """Smaže kontakt; contacts-cpp kaskádově odstraní i jeho telefonní čísla."""
    return await _proxy(request, f"{CONTACTS_URL}/contacts/{contact_id}")


# Search ---------------------------------------------------------------------

@app.get("/api/search")
async def search(request: Request, q: str = ""):
    """Přesměruje fulltext dotaz do search-rust, který udržuje invertovaný index."""
    return await _proxy(request, f"{SEARCH_URL}/search", params={"q": q})


# Database tables ------------------------------------------------------------

@app.get("/api/db")
async def db_tables(request: Request):
    """Vrátí raw obsah PostgreSQL tabulek pro diagnostický pohled v UI."""
    return await _proxy(request, f"{CONTACTS_URL}/db/tables")


# Dedup ----------------------------------------------------------------------

@app.get("/api/dedup")
async def dedup(request: Request, threshold: float = 0.85):
    """Najde potenciální duplicity; threshold určuje minimální míru podobnosti (0–1)."""
    return await _proxy(request, f"{CONTACTS_URL}/dedup", params={"threshold": threshold})


# Analytics ------------------------------------------------------------------

@app.get("/api/analytics")
async def analytics(request: Request):
    """Vrátí agregované metriky (rozdělení kategorií, počty atd.) z contacts-cpp."""
    return await _proxy(request, f"{CONTACTS_URL}/analytics")


# vCard export / import ------------------------------------------------------

@app.get("/api/export/vcard")
async def export_vcard(request: Request):
    """Exportuje všechny kontakty jako jeden vCard soubor (.vcf)."""
    return await _proxy(request, f"{CONTACTS_URL}/export/vcard")


@app.post("/api/import/vcard")
async def import_vcard(request: Request):
    """Přijme vCard soubor a naimportuje kontakty dávkově do contacts-cpp."""
    return await _proxy(request, f"{CONTACTS_URL}/import/vcard")


# Services / topology --------------------------------------------------------

@app.get("/api/services")
async def services(request: Request):
    """Vrátí registry registrovaných služeb přímo z gateway (včetně health statusu)."""
    return await _proxy(request, f"{GATEWAY_URL}/services")


@app.get("/api/topology")
async def topology(request: Request):
    """Vrátí graf závislostí mezi službami, jak ho gateway nasbírala z provozních volání."""
    return await _proxy(request, f"{GATEWAY_URL}/topology")


# Stats (parallel fan-out) ---------------------------------------------------

async def _fetch_stats(name: str, base_url: str) -> dict:
    """Načte statistiky z jedné služby a obalí je jejím jménem.

    Chyba sítě nebo timeout vrátí degradovaný záznam místo výjimky,
    aby jeden nedostupný backend nezabránil zobrazení ostatních.
    """
    try:
        r = await _client.get(f"{base_url}/stats", timeout=5.0)
        data = r.json() if r.status_code == 200 else {}
        return {"service": name, **data}
    except Exception as exc:
        return {"service": name, "error": str(exc)}


@app.get("/api/stats")
async def stats():
    """Agreguje statistiky ze všech backendových služeb do jedné odpovědi."""
    targets = [
        ("contacts-cpp", CONTACTS_URL),
        ("search-rust", SEARCH_URL),
        ("gateway-go", GATEWAY_URL),
    ]
    # Voláme všechny tři služby souběžně — celková latence odpovídá nejpomalejší
    # z nich, ne jejich součtu. Při sekvenčním volání by každý timeout 5 s
    # znamenal až 15 s čekání pro uživatele.
    results = await asyncio.gather(
        *[_fetch_stats(name, url) for name, url in targets]
    )
    return JSONResponse(content=list(results))


# Static files (must be mounted last so explicit routes take priority) -------
app.mount("/", StaticFiles(directory=str(STATIC_DIR), html=True), name="static")
