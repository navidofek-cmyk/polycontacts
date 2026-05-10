"""Tests for bff-python FastAPI service.

All upstream HTTP calls are intercepted via a custom httpx.MockTransport so
that no real services need to be running during the test suite.

Strategy
--------
The module-level ``_client`` in ``app.main`` is created inside the lifespan
context manager.  We patch ``httpx.AsyncClient`` in that module so that when
the lifespan runs it receives our mock transport instead of making real network
calls.  The patch must be applied *before* ``TestClient.__enter__`` triggers
the lifespan startup.
"""
from __future__ import annotations

import json
from contextlib import asynccontextmanager
from typing import Callable
from unittest.mock import patch

import httpx
import pytest
from fastapi.testclient import TestClient

import app.main as main_module
from app.main import app, CONTACTS_URL, SEARCH_URL, GATEWAY_URL


# ---------------------------------------------------------------------------
# Mock transport helpers
# ---------------------------------------------------------------------------

class _AsyncMockTransport(httpx.AsyncBaseTransport):
    def __init__(self, handler: Callable[[httpx.Request], httpx.Response]):
        self._handler = handler

    async def handle_async_request(self, request: httpx.Request) -> httpx.Response:
        return self._handler(request)


def _json_response(data, status_code: int = 200) -> httpx.Response:
    return httpx.Response(
        status_code=status_code,
        headers={"content-type": "application/json"},
        content=json.dumps(data).encode(),
    )


def _make_async_client(handler: Callable[[httpx.Request], httpx.Response]) -> httpx.AsyncClient:
    return httpx.AsyncClient(transport=_AsyncMockTransport(handler), timeout=10.0)


# ---------------------------------------------------------------------------
# Default request handler
# ---------------------------------------------------------------------------

def _default_handler(request: httpx.Request) -> httpx.Response:
    url = str(request.url)
    path = request.url.path

    # Gateway self-registration (lifespan POST)
    if url.startswith(GATEWAY_URL) and path == "/services" and request.method == "POST":
        return _json_response({"ok": True})

    if url.startswith(CONTACTS_URL):
        if path == "/contacts":
            if request.method == "GET":
                return _json_response([{"id": 1, "name": "Jana"}])
            if request.method == "POST":
                return _json_response({"id": 2, "name": "Petr"}, status_code=201)
        if path.startswith("/contacts/"):
            if request.method == "PUT":
                return _json_response({"id": 1, "name": "Updated"})
            if request.method == "DELETE":
                return httpx.Response(status_code=204)
        if path == "/stats":
            return _json_response({"total": 42})

    if url.startswith(SEARCH_URL):
        if path == "/search":
            return _json_response([{"id": 1, "name": "Jana"}])
        if path == "/stats":
            return _json_response({"indexed": 10})

    if url.startswith(GATEWAY_URL):
        if path == "/services":
            return _json_response([{"name": "contacts-cpp"}])
        if path == "/topology":
            return _json_response({"nodes": []})
        if path == "/stats":
            return _json_response({"uptime": 99})

    return httpx.Response(status_code=404, content=b"not found")


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

def _patched_client(handler: Callable[[httpx.Request], httpx.Response] = _default_handler):
    """Return a context manager that patches httpx.AsyncClient in app.main."""
    mock_client = _make_async_client(handler)

    # We patch the class so that when lifespan calls
    # ``httpx.AsyncClient(timeout=10.0)`` it gets our pre-built instance back.
    class _FakeAsyncClient:
        def __new__(cls, *args, **kwargs):  # noqa: D102
            return mock_client

    return patch.object(httpx, "AsyncClient", _FakeAsyncClient)


@pytest.fixture()
def client():
    """TestClient with upstream HTTP calls intercepted by the mock transport."""
    with _patched_client():
        with TestClient(app) as tc:
            yield tc


# ---------------------------------------------------------------------------
# Tests: /health
# ---------------------------------------------------------------------------

class TestHealth:
    def test_returns_200(self, client: TestClient):
        response = client.get("/health")
        assert response.status_code == 200

    def test_returns_status_ok(self, client: TestClient):
        data = client.get("/health").json()
        assert data["status"] == "ok"


# ---------------------------------------------------------------------------
# Tests: /api/contacts  (GET)
# ---------------------------------------------------------------------------

class TestGetContacts:
    def test_proxies_to_contacts_url(self, client: TestClient):
        response = client.get("/api/contacts")
        assert response.status_code == 200
        data = response.json()
        assert isinstance(data, list)
        assert data[0]["name"] == "Jana"

    def test_forwards_q_param_upstream(self):
        captured: list[httpx.Request] = []

        def handler(request: httpx.Request) -> httpx.Response:
            captured.append(request)
            return _default_handler(request)

        with _patched_client(handler):
            with TestClient(app) as tc:
                tc.get("/api/contacts?q=jana")

        contacts_reqs = [r for r in captured if r.url.path == "/contacts" and r.method == "GET"]
        assert contacts_reqs, "No GET /contacts forwarded upstream"
        assert "jana" in str(contacts_reqs[0].url)


# ---------------------------------------------------------------------------
# Tests: /api/contacts  (POST)
# ---------------------------------------------------------------------------

class TestPostContacts:
    def test_proxies_to_contacts_url(self, client: TestClient):
        payload = {"name": "Petr", "email": "petr@example.com"}
        response = client.post("/api/contacts", json=payload)
        assert response.status_code == 201
        assert response.json()["name"] == "Petr"

    def test_request_is_forwarded_as_post(self):
        captured: list[httpx.Request] = []

        def handler(request: httpx.Request) -> httpx.Response:
            captured.append(request)
            return _default_handler(request)

        with _patched_client(handler):
            with TestClient(app) as tc:
                tc.post("/api/contacts", json={"name": "X"})

        post_reqs = [r for r in captured if r.url.path == "/contacts" and r.method == "POST"]
        assert post_reqs, "No POST /contacts forwarded upstream"


# ---------------------------------------------------------------------------
# Tests: /api/search
# ---------------------------------------------------------------------------

class TestSearch:
    def test_proxies_returns_list(self, client: TestClient):
        response = client.get("/api/search?q=jana")
        assert response.status_code == 200
        assert isinstance(response.json(), list)

    def test_forwards_q_param(self):
        captured: list[httpx.Request] = []

        def handler(request: httpx.Request) -> httpx.Response:
            captured.append(request)
            return _default_handler(request)

        with _patched_client(handler):
            with TestClient(app) as tc:
                tc.get("/api/search?q=jana")

        search_reqs = [r for r in captured if r.url.path == "/search"]
        assert search_reqs, "No /search request forwarded upstream"
        assert "jana" in str(search_reqs[0].url)


# ---------------------------------------------------------------------------
# Tests: /api/stats  (parallel fan-out)
# ---------------------------------------------------------------------------

class TestStats:
    def test_returns_list(self, client: TestClient):
        response = client.get("/api/stats")
        assert response.status_code == 200
        assert isinstance(response.json(), list)

    def test_includes_all_three_services(self, client: TestClient):
        data = client.get("/api/stats").json()
        names = {item["service"] for item in data}
        assert "contacts-cpp" in names
        assert "search-rust" in names
        assert "gateway-go" in names

    def test_merges_upstream_data(self, client: TestClient):
        data = client.get("/api/stats").json()
        by_name = {item["service"]: item for item in data}
        assert by_name["contacts-cpp"].get("total") == 42
        assert by_name["search-rust"].get("indexed") == 10
        assert by_name["gateway-go"].get("uptime") == 99

    def test_handles_upstream_error_gracefully(self):
        def failing_handler(request: httpx.Request) -> httpx.Response:
            if request.url.path == "/stats":
                raise httpx.ConnectError("connection refused")
            return _default_handler(request)

        with _patched_client(failing_handler):
            with TestClient(app) as tc:
                response = tc.get("/api/stats")

        assert response.status_code == 200
        data = response.json()
        assert isinstance(data, list)
        assert len(data) == 3
        for item in data:
            assert "error" in item


# ---------------------------------------------------------------------------
# Tests: /api/services
# ---------------------------------------------------------------------------

class TestServices:
    def test_proxies_to_gateway(self, client: TestClient):
        response = client.get("/api/services")
        assert response.status_code == 200
        data = response.json()
        assert isinstance(data, list)
        assert data[0]["name"] == "contacts-cpp"

    def test_request_forwarded_to_gateway_url(self):
        captured: list[httpx.Request] = []

        def handler(request: httpx.Request) -> httpx.Response:
            captured.append(request)
            return _default_handler(request)

        with _patched_client(handler):
            with TestClient(app) as tc:
                tc.get("/api/services")

        gateway_reqs = [
            r for r in captured
            if str(r.url).startswith(GATEWAY_URL) and r.url.path == "/services" and r.method == "GET"
        ]
        assert gateway_reqs, "No GET /services forwarded to gateway"


# ---------------------------------------------------------------------------
# Tests: /api/topology
# ---------------------------------------------------------------------------

class TestTopology:
    def test_proxies_to_gateway(self, client: TestClient):
        response = client.get("/api/topology")
        assert response.status_code == 200
        data = response.json()
        assert "nodes" in data

    def test_request_forwarded_to_gateway_url(self):
        captured: list[httpx.Request] = []

        def handler(request: httpx.Request) -> httpx.Response:
            captured.append(request)
            return _default_handler(request)

        with _patched_client(handler):
            with TestClient(app) as tc:
                tc.get("/api/topology")

        topo_reqs = [
            r for r in captured
            if str(r.url).startswith(GATEWAY_URL) and r.url.path == "/topology"
        ]
        assert topo_reqs, "No /topology request forwarded to gateway"
