from fastapi import FastAPI, HTTPException
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse
from pydantic import BaseModel
from enum import Enum
from pathlib import Path
from typing import Optional

app = FastAPI(title="Contacts API")

# ── modely ────────────────────────────────────────────────────────────────────

class Category(str, Enum):
    friend = "Friend"
    work   = "Work"
    family = "Family"
    other  = "Other"

class PhoneNumber(BaseModel):
    label:  str
    number: str

class Contact(BaseModel):
    first_name: str
    last_name:  str
    email:      Optional[str] = None
    phones:     list[PhoneNumber] = []
    category:   Category = Category.other

class ContactIn(Contact):
    pass

# ── in-memory store ───────────────────────────────────────────────────────────

db: list[Contact] = [
    Contact(first_name="Jana",  last_name="Nováková", email="jana@example.com",
            phones=[PhoneNumber(label="mobil", number="+420 601 111 222")],
            category=Category.friend),
    Contact(first_name="Petr",  last_name="Svoboda",
            phones=[PhoneNumber(label="práce", number="+420 602 333 444"),
                    PhoneNumber(label="mobil", number="+420 777 555 666")],
            category=Category.work),
    Contact(first_name="Marie", last_name="Svoboda", email="marie@example.com",
            phones=[PhoneNumber(label="domů", number="+420 603 777 888")],
            category=Category.family),
    Contact(first_name="Tomáš", last_name="Dvořák",  email="tomas@work.cz",
            phones=[PhoneNumber(label="mobil", number="+420 604 999 000")],
            category=Category.work),
]

# ── endpoints ─────────────────────────────────────────────────────────────────

@app.get("/contacts", response_model=list[Contact])
def list_contacts(q: str = ""):
    if not q:
        return sorted(db, key=lambda c: (c.last_name, c.first_name))
    q_low = q.lower()
    return [c for c in db if q_low in c.first_name.lower()
                                or q_low in c.last_name.lower()
                                or (c.email and q_low in c.email.lower())]

@app.post("/contacts", response_model=Contact, status_code=201)
def add_contact(contact: ContactIn):
    db.append(Contact(**contact.model_dump()))
    return db[-1]

@app.delete("/contacts/{full_name}")
def remove_contact(full_name: str):
    before = len(db)
    db[:] = [c for c in db if f"{c.first_name} {c.last_name}" != full_name]
    if len(db) == before:
        raise HTTPException(404, "Kontakt nenalezen")
    return {"removed": before - len(db)}

# ── statické soubory ──────────────────────────────────────────────────────────

static_dir = Path(__file__).parent / "static"
app.mount("/static", StaticFiles(directory=static_dir), name="static")

@app.get("/")
def index():
    return FileResponse(static_dir / "index.html")
