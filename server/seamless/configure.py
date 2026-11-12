#!/usr/bin/env python3
"""Prepare the private room for the Docker deployment."""
import base64
import binascii
import getpass
import hashlib
import hmac
import os
from pathlib import Path
import re

PREFIX = "LANTERN-ROOM:"


def decode_invite(code):
    code = code.strip()
    if not code.startswith(PREFIX) or len(code) > 8192:
        raise ValueError("Use the complete LANTERN-ROOM: invitation code copied by the host.")
    body = code[len(PREFIX):]
    if not re.fullmatch(r"[A-Za-z0-9_-]+", body):
        raise ValueError("Invalid invitation code.")
    try:
        parts = base64.b64decode(body + "=" * (-len(body) % 4), altchars=b"-_", validate=True).decode("utf-8").split("\n")
    except (ValueError, UnicodeError, binascii.Error) as exc:
        raise ValueError("Invalid invitation code.") from exc
    if len(parts) not in (2, 3) or not parts[0] or not parts[1] or any("\0" in p or "\r" in p for p in parts):
        raise ValueError("The invitation is missing its room/password or has an invalid format.")
    return parts[0], parts[1]


def room_id(name, password):
    # Same derivation as engine/gpu/bbnet/room/room_crypto.cpp.
    data = b"lantern-room\0" + name.encode("utf-8") + b"\0" + password.encode("utf-8")
    master = hashlib.pbkdf2_hmac("sha256", data, b"lantern-seamless-coop", 200000, 32)
    return hmac.new(master, b"room id", hashlib.sha256).hexdigest()


def domain_name(value):
    value = value.strip().lower()
    labels = value.split(".")
    if (len(value) > 253 or len(labels) < 2 or value.replace(".", "").isdigit()
            or not all(re.fullmatch(r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?", label) for label in labels)):
        raise ValueError("Set RELAY_DOMAIN to a DNS name without https:// or a port.")
    return value


def make_invite(name, password, domain):
    body = f"{name}\n{password}\nhttps://{domain}".encode("utf-8")
    return PREFIX + base64.urlsafe_b64encode(body).decode("ascii").rstrip("=")


def write_private(path, text):
    with path.open("w", encoding="utf-8", newline="\n") as output:
        os.chmod(path, 0o600)
        output.write(text)


def update_environment(path, values):
    """Update generated values while keeping the operator's settings and comments."""
    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    remaining = dict(values)
    result = []
    for line in lines:
        match = re.match(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*=", line)
        key = match.group(1) if match else None
        if key in values:
            if key in remaining:
                result.append(f"{key}={remaining.pop(key)}")
        else:
            result.append(line)
    result.extend(f"{key}={value}" for key, value in remaining.items())
    write_private(path, "\n".join(result) + "\n")


def main():
    directory = Path(__file__).resolve().parent
    invite = directory / "invite.txt"
    domain = domain_name(os.environ.get("RELAY_DOMAIN", ""))
    if domain == "example.com" or domain.endswith(".example.com"):
        raise ValueError("Replace RELAY_DOMAIN in .env with your VPS domain.")
    code = invite.read_text(encoding="utf-8") if invite.exists() else getpass.getpass("Paste the HOST invitation code (input hidden): ")
    name, password = decode_invite(code)
    update_environment(directory / ".env", {"RELAY_DOMAIN": domain, "ROOM_ID": room_id(name, password)})
    write_private(invite, make_invite(name, password, domain) + "\n")
    print("Room configured. Share invite.txt with your guests.")
    print("On the host: seamless_server=https://" + domain)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, EOFError) as exc:
        raise SystemExit(f"Error: {exc}")
