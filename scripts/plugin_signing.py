#!/usr/bin/env python3
"""Plugin signatures (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md, section 11; the owner's
decision 8): this PC's plugin signing key, and plugin.sig - a signed statement of a plugin folder's SHA-256s.

The key is an Ed25519 key pair whose private half stays out of every install and data folder (default_key_path):
Windows protects its seed with DPAPI for this user, elsewhere it is a 0600 file in a 0700 folder, and either may also be
sealed with a passphrase (scrypt, then the seed XORed with the derived key's first 32 bytes and an HMAC-SHA256 under its
last 32, so a wrong passphrase is refused). The signature is the release envelope's form (scripts/release/sign.py,
secure_frontend/src/release_manifest.hpp): the statement, the --STRATA-SIGNATURE-V1 line, and the base64 Ed25519
signature over the statement's bytes, made with the RFC 8032 section 6 reference code sign.py carries. The server
verifies it with its own RFC 8032 verifier (secure_frontend_ed25519). Standard library only.

  plugin_signing.py --show [--key PATH]                    the public key (for another install's owner to add)
  plugin_signing.py --sign FOLDER [--platform P] [--key PATH] [--passphrase]
"""
from __future__ import annotations

import argparse
import base64
import getpass
import hashlib
import hmac
import importlib.util
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATEMENT_SCHEMA = "m-strata.plugin-signature/1"
KEY_SCHEMA = "m-strata.signing-key/1"
FRAMING = b"\n--STRATA-SIGNATURE-V1\n"
SIGNATURE_FILE = "plugin.sig"
KEY_FILE = "plugin-signing-key.json"
# scrypt for the passphrase: 16 MiB of memory, within hashlib's default limit
_SCRYPT = {"n": 2 ** 14, "r": 8, "p": 1, "dklen": 64}


def _release_signer():
    spec = importlib.util.spec_from_file_location("strata_release_sign", ROOT / "scripts" / "release" / "sign.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_SIGN = _release_signer()


class SigningKeyError(Exception):
    """A key file that is not one, a wrong or missing passphrase: the message says which."""


@dataclass(frozen=True)
class SigningKey:
    seed: bytes
    public_hex: str
    path: Path


def default_key_path(platform: str, environ: dict[str, str] | None = None) -> Path:
    """Where this user's plugin signing key lives: outside every install and data folder."""
    env = os.environ if environ is None else environ
    if platform == "windows":
        base = env.get("APPDATA") or str(Path.home() / "AppData" / "Roaming")
        return Path(base) / "M-Strata" / KEY_FILE
    base = env.get("XDG_CONFIG_HOME") or str(Path.home() / ".config")
    return Path(base) / "m-strata" / KEY_FILE


# ---- DPAPI (Windows): the seed protected for this user, as the server keeps its secrets ----------------------------

def _dpapi(data: bytes, protect: bool) -> bytes:
    import ctypes
    import ctypes.wintypes

    class Blob(ctypes.Structure):
        _fields_ = [("cbData", ctypes.wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    buffer = ctypes.create_string_buffer(data, len(data))
    blob_in = Blob(len(data), ctypes.cast(buffer, ctypes.POINTER(ctypes.c_char)))
    blob_out = Blob()
    crypt32 = ctypes.windll.crypt32
    ok = (crypt32.CryptProtectData(ctypes.byref(blob_in), "m-strata plugin signing key", None, None, None, 1, ctypes.byref(blob_out))
          if protect else
          crypt32.CryptUnprotectData(ctypes.byref(blob_in), None, None, None, None, 1, ctypes.byref(blob_out)))
    if not ok:
        raise SigningKeyError("the signing key could not be " + ("protected" if protect else "unprotected") +
                              " with Windows DPAPI (another user's key?)")
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(blob_out.pbData)


def _seal(seed: bytes, passphrase: str, salt: bytes) -> tuple[bytes, str]:
    derived = hashlib.scrypt(passphrase.encode("utf-8"), salt=salt, **_SCRYPT)
    sealed = bytes(a ^ b for a, b in zip(seed, derived[:32]))
    return sealed, hmac.new(derived[32:], salt + sealed, hashlib.sha256).hexdigest()


def _unseal(sealed: bytes, passphrase: str, salt: bytes, check: str) -> bytes:
    derived = hashlib.scrypt(passphrase.encode("utf-8"), salt=salt, **_SCRYPT)
    if not hmac.compare_digest(hmac.new(derived[32:], salt + sealed, hashlib.sha256).hexdigest(), check):
        raise SigningKeyError("the passphrase is not the signing key's")
    return bytes(a ^ b for a, b in zip(sealed, derived[:32]))


def create_key(path: Path, platform: str, passphrase: str | None = None, seed: bytes | None = None) -> SigningKey:
    """A new key pair at `path` (which must not exist), its seed protected as this platform keeps secrets."""
    seed = os.urandom(32) if seed is None else seed
    public_hex = _SIGN.secret_to_public(seed).hex()
    record = {"schema": KEY_SCHEMA, "public": public_hex, "protection": "dpapi" if platform == "windows" else "file",
              "passphrase": passphrase is not None}
    stored = seed
    if passphrase is not None:
        salt = os.urandom(16)
        stored, record["check"] = _seal(seed, passphrase, salt)
        record["salt"] = salt.hex()
    record["seed"] = base64.b64encode(_dpapi(stored, True)).decode("ascii") if platform == "windows" else stored.hex()
    text = json.dumps(record, indent=2) + "\n"
    if platform == "windows":
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "x", encoding="utf-8", newline="\n") as out:
            out.write(text)
    else:
        path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(path.parent, 0o700)
        handle = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as out:
            out.write(text)
    return SigningKey(seed=seed, public_hex=public_hex, path=path)


def load_key(path: Path, passphrase: str | None = None) -> SigningKey:
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise SigningKeyError(f"{path}: not a signing key file ({error})") from None
    if not isinstance(record, dict) or record.get("schema") != KEY_SCHEMA or not re.fullmatch(r"[0-9a-f]{64}", str(record.get("public"))):
        raise SigningKeyError(f"{path}: not a signing key file")
    try:
        if record.get("protection") == "dpapi":
            stored = _dpapi(base64.b64decode(str(record.get("seed")), validate=True), False)
        elif record.get("protection") == "file":
            stored = bytes.fromhex(str(record.get("seed")))
        else:
            raise SigningKeyError(f"{path}: not a signing key file")
    except ValueError:
        raise SigningKeyError(f"{path}: not a signing key file") from None
    if record.get("passphrase") is True:
        if passphrase is None:
            raise SigningKeyError(f"{path}: the signing key needs its passphrase (--signing-passphrase)")
        seed = _unseal(stored, passphrase, bytes.fromhex(str(record.get("salt"))), str(record.get("check")))
    else:
        seed = stored
    if len(seed) != 32 or _SIGN.secret_to_public(seed).hex() != record["public"]:
        raise SigningKeyError(f"{path}: the seed does not match the key's public half")
    return SigningKey(seed=seed, public_hex=record["public"], path=path)


def load_or_create_key(path: Path, platform: str, passphrase: str | None = None) -> tuple[SigningKey, bool]:
    if path.exists():
        return load_key(path, passphrase), False
    return create_key(path, platform, passphrase), True


# ---- the signature ---------------------------------------------------------------------------------------------------

def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256(path: Path) -> str:
    return _sha256_bytes(path.read_bytes())


def statement(public_hex: str, plugin_id: str, platform: str, manifest_sha256: str, files: dict[str, str]) -> bytes:
    """What a plugin's signature covers, as the server rebuilds it from the folder (section 11)."""
    lines = [STATEMENT_SCHEMA, f"key {public_hex}", f"id {plugin_id}", f"platform {platform}", f"manifest {manifest_sha256}"]
    lines += [f"file {name} {files[name]}" for name in sorted(files)]
    return ("\n".join(lines) + "\n").encode("utf-8")


def folder_statement(folder: Path, platform: str, public_hex: str) -> bytes:
    """The statement for a plugin folder as it is now: its manifest and each file it lists for `platform`."""
    manifest_path = folder / "plugin.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    plugin_id = folder.name
    if manifest.get("id") != plugin_id:
        raise SystemExit(f"{folder}: its plugin.json's id is not its folder's name")
    files = {}
    for name in manifest.get("files", {}).get(platform, []):
        if not (folder / name).is_file():
            raise SystemExit(f"{folder}: {name}, which plugin.json lists for {platform}, is not there")
        files[name] = _sha256(folder / name)
    return statement(public_hex, plugin_id, platform, _sha256(manifest_path), files)


def envelope(statement_bytes: bytes, seed: bytes) -> bytes:
    return _SIGN.envelope(statement_bytes, seed)


def sign_plugin(folder: Path, platform: str, key: SigningKey) -> Path:
    """Write <folder>/plugin.sig: the folder's statement signed with `key`."""
    target = folder / SIGNATURE_FILE
    target.write_bytes(envelope(folder_statement(folder, platform, key.public_hex), key.seed))
    return target


def verify_plugin(folder: Path, public_key: bytes) -> bool:
    """Whether <folder>/plugin.sig verifies under `public_key` (the statement's own check is the server's)."""
    return _SIGN.verify_envelope((folder / SIGNATURE_FILE).read_bytes(), public_key)


def is_sealed(path: Path) -> bool:
    """Whether the key file at `path` is sealed with a passphrase (False too when there is none, or it is not one: load_key
    says which)."""
    try:
        return json.loads(path.read_text(encoding="utf-8")).get("passphrase") is True
    except (OSError, ValueError, AttributeError):
        return False


def ask_passphrase(path: Path, wanted: bool) -> str | None:
    """The passphrase for the key at `path`, asked for only where it is used: twice for a key about to be made, once for
    one sealed with it. A key there without one is used as it is - a passphrase seals only a key being made - and that
    is said (on stderr)."""
    if not wanted:
        return None
    if path.exists() and not is_sealed(path):
        print(f"{path}: the signing key is not sealed with a passphrase, and a key that exists is not sealed afresh: "
              "it is used as it is", file=sys.stderr)
        return None
    creating = not path.exists()
    first = getpass.getpass("plugin signing key passphrase: ")
    if creating and getpass.getpass("again: ") != first:
        raise SystemExit("the passphrases differ")
    return first


def main(argv: list[str]) -> int:
    platform = "windows" if os.name == "nt" else "linux"
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--key", type=Path, default=None, help="the signing key file (default: this user's)")
    parser.add_argument("--show", action="store_true", help="print the key's public half")
    parser.add_argument("--sign", type=Path, default=None, help="a plugin folder to sign")
    parser.add_argument("--platform", choices=("windows", "linux"), default=platform, help="the platform the folder is for")
    parser.add_argument("--passphrase", action="store_true", help="the key is (or, new, will be) sealed with a passphrase")
    args = parser.parse_args(argv)
    path = args.key or default_key_path(platform)
    try:
        if args.show:
            print(load_key(path, ask_passphrase(path, args.passphrase)).public_hex if path.exists() else f"no signing key at {path}")
            return 0 if path.exists() else 1
        if args.sign is None:
            parser.error("--show or --sign FOLDER")
        key, created = load_or_create_key(path, platform, ask_passphrase(path, args.passphrase))
    except SigningKeyError as error:
        raise SystemExit(str(error)) from None
    if created:
        print(f"created the plugin signing key at {path}")
    written = sign_plugin(args.sign, args.platform, key)
    print(f"signed {args.sign.name} for {args.platform} with key {key.public_hex}: {written}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
