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
    global _client
    _client = httpx.AsyncClient(timeout=10.0)

    # Register self with gateway
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
    """Forward a request to *url* and return the upstream response."""
    body = await request.body()
    upstream = await _client.request(
        method=request.method,
        url=url,
        content=body,
        headers={
            k: v
            for k, v in request.headers.items()
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
    return {"status": "ok", "service": "bff-python"}


@app.get("/")
async def index():
    return FileResponse(STATIC_DIR / "index.html")


# Contacts -------------------------------------------------------------------

@app.get("/api/contacts")
async def get_contacts(request: Request, q: str = ""):
    return await _proxy(request, f"{CONTACTS_URL}/contacts", params={"q": q})


@app.post("/api/contacts")
async def create_contact(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/contacts")


@app.put("/api/contacts/{contact_id:path}")
async def update_contact(request: Request, contact_id: str):
    return await _proxy(request, f"{CONTACTS_URL}/contacts/{contact_id}")


@app.delete("/api/contacts/{contact_id:path}")
async def delete_contact(request: Request, contact_id: str):
    return await _proxy(request, f"{CONTACTS_URL}/contacts/{contact_id}")


# Search ---------------------------------------------------------------------

@app.get("/api/search")
async def search(request: Request, q: str = ""):
    return await _proxy(request, f"{SEARCH_URL}/search", params={"q": q})


# Database tables ------------------------------------------------------------

@app.get("/api/db")
async def db_tables(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/db/tables")


# Dedup ----------------------------------------------------------------------

@app.get("/api/dedup")
async def dedup(request: Request, threshold: float = 0.85):
    return await _proxy(request, f"{CONTACTS_URL}/dedup", params={"threshold": threshold})


# Analytics ------------------------------------------------------------------

@app.get("/api/analytics")
async def analytics(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/analytics")


# vCard export / import ------------------------------------------------------

@app.get("/api/export/vcard")
async def export_vcard(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/export/vcard")


@app.post("/api/import/vcard")
async def import_vcard(request: Request):
    return await _proxy(request, f"{CONTACTS_URL}/import/vcard")


# Services / topology --------------------------------------------------------

@app.get("/api/services")
async def services(request: Request):
    return await _proxy(request, f"{GATEWAY_URL}/services")


@app.get("/api/topology")
async def topology(request: Request):
    return await _proxy(request, f"{GATEWAY_URL}/topology")


# Stats (parallel fan-out) ---------------------------------------------------

async def _fetch_stats(name: str, base_url: str) -> dict:
    try:
        r = await _client.get(f"{base_url}/stats", timeout=5.0)
        data = r.json() if r.status_code == 200 else {}
        return {"service": name, **data}
    except Exception as exc:
        return {"service": name, "error": str(exc)}


@app.get("/api/stats")
async def stats():
    targets = [
        ("contacts-cpp", CONTACTS_URL),
        ("search-rust", SEARCH_URL),
        ("gateway-go", GATEWAY_URL),
    ]
    results = await asyncio.gather(
        *[_fetch_stats(name, url) for name, url in targets]
    )
    return JSONResponse(content=list(results))


# Static files (must be mounted last so explicit routes take priority) -------
app.mount("/", StaticFiles(directory=str(STATIC_DIR), html=True), name="static")
