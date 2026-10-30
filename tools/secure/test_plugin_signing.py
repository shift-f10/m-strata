"""Plugin signatures (spec 2026-10-30 section 11; the owner's decision 8): scripts/plugin_signing.py - the statement a
signature covers, a plugin folder signed and verified, this user's signing key (DPAPI on Windows, 0600 elsewhere, a
passphrase optional), and the fixtures the C++ verifier is tested against, which this signer must reproduce.

Run `python tools/secure/test_plugin_signing.py --write` to write the fixtures again (only when the format changes)."""
from __future__ import annotations

import importlib.util
import json
import os
import stat
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def _load():
    spec = importlib.util.spec_from_file_location("plugin_signing", ROOT / "scripts" / "plugin_signing.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules["plugin_signing"] = module  # (its dataclass resolves its annotations through it)
    spec.loader.exec_module(module)
    return module


signing = _load()
PLATFORM = "windows" if os.name == "nt" else "linux"
# RFC 8032 section 7.1's TEST 1 key: public test material, never a key anything trusts
RFC_SEED = bytes.fromhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
RFC_PUBLIC = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
# RFC 8032 section 7.1's TEST 2 key: a second, for "signed by another key"
OTHER_SEED = bytes.fromhex("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb")

FIXTURES = ROOT / "secure_frontend" / "tests" / "fixtures" / "plugins" / "signedtool"
FIXTURE_MANIFEST = ('{"schema": "m-strata.plugin/1", "id": "signedtool", "kind": "tool", "name": "Signed tool", "version": "1.0.0",\n'
                    '  "publisher": "Example Labs", "license": "MIT", "about": "A signed stand-in.",\n'
                    '  "files": {"windows": ["signed-tool.exe"], "linux": ["signed-tool"]},\n'
                    '  "tool": {"program": {"windows": "signed-tool.exe", "linux": "signed-tool"}}}\n')
FIXTURE_PROGRAM = b"signed tool program\n"


def rfc_key(path: Path = Path("rfc-test-key")):
    return signing.SigningKey(seed=RFC_SEED, public_hex=RFC_PUBLIC, path=path)


def plugin_folder(base: Path, platform: str = PLATFORM) -> Path:
    folder = base / "signedtool"
    folder.mkdir(parents=True)
    (folder / "plugin.json").write_bytes(FIXTURE_MANIFEST.encode("utf-8"))
    (folder / ("signed-tool.exe" if platform == "windows" else "signed-tool")).write_bytes(FIXTURE_PROGRAM)
    return folder


def test_the_statement_is_the_contracts():
    text = signing.statement(RFC_PUBLIC, "lookup", "linux", "a" * 64, {"lookup": "b" * 64, "data/x.bin": "c" * 64})
    assert text == ("m-strata.plugin-signature/1\nkey " + RFC_PUBLIC + "\nid lookup\nplatform linux\nmanifest " + "a" * 64 +
                    "\nfile data/x.bin " + "c" * 64 + "\nfile lookup " + "b" * 64 + "\n").encode("utf-8")
    assert signing.statement(RFC_PUBLIC, "debugging", "windows", "d" * 64, {}).endswith(b"manifest " + b"d" * 64 + b"\n")


def test_a_folder_is_signed_and_verifies(tmp_path):
    folder = plugin_folder(tmp_path)
    written = signing.sign_plugin(folder, PLATFORM, rfc_key())
    assert written == folder / "plugin.sig"
    envelope = written.read_bytes()
    statement, _, rest = envelope.partition(signing.FRAMING)
    assert statement == signing.folder_statement(folder, PLATFORM, RFC_PUBLIC)
    assert statement.startswith(b"m-strata.plugin-signature/1\nkey " + RFC_PUBLIC.encode() + b"\nid signedtool\nplatform " + PLATFORM.encode())
    assert len(rest.rstrip(b"\n")) == 88, "one base64 Ed25519 signature line"
    assert signing.verify_plugin(folder, bytes.fromhex(RFC_PUBLIC))
    other_public = signing._SIGN.secret_to_public(OTHER_SEED)
    assert not signing.verify_plugin(folder, other_public), "another key's public half does not verify it"
    flipped = bytearray(envelope)
    flipped[len(statement) + len(signing.FRAMING) + 4] ^= 0x01
    written.write_bytes(bytes(flipped))
    assert not signing.verify_plugin(folder, bytes.fromhex(RFC_PUBLIC)), "a changed signature does not verify"
    # a changed file changes the statement the server rebuilds from the folder: the old signature no longer covers it
    signing.sign_plugin(folder, PLATFORM, rfc_key())
    program = folder / ("signed-tool.exe" if PLATFORM == "windows" else "signed-tool")
    program.write_bytes(b"something else\n")
    assert signing.folder_statement(folder, PLATFORM, RFC_PUBLIC) != (folder / "plugin.sig").read_bytes().partition(signing.FRAMING)[0]


def test_a_folder_whose_files_are_not_there_is_not_signed(tmp_path):
    folder = plugin_folder(tmp_path)
    (folder / ("signed-tool.exe" if PLATFORM == "windows" else "signed-tool")).unlink()
    with pytest.raises(SystemExit, match="is not there"):
        signing.sign_plugin(folder, PLATFORM, rfc_key())
    assert not (folder / "plugin.sig").exists()


def test_a_key_is_created_kept_private_and_loaded(tmp_path):
    path = tmp_path / "keys" / "plugin-signing-key.json"
    key = signing.create_key(path, PLATFORM)
    record = json.loads(path.read_text(encoding="utf-8"))
    assert record["schema"] == "m-strata.signing-key/1" and record["public"] == key.public_hex
    assert record["public"] == signing._SIGN.secret_to_public(key.seed).hex()
    assert record["protection"] == ("dpapi" if PLATFORM == "windows" else "file") and record["passphrase"] is False
    if PLATFORM == "windows":
        assert key.seed.hex() not in path.read_text(encoding="utf-8"), "the seed is never in the clear on Windows"
    else:
        assert stat.S_IMODE(path.stat().st_mode) == 0o600 and stat.S_IMODE(path.parent.stat().st_mode) == 0o700
    loaded = signing.load_key(path)
    assert loaded.seed == key.seed and loaded.public_hex == key.public_hex
    with pytest.raises(FileExistsError):
        signing.create_key(path, PLATFORM)
    again, created = signing.load_or_create_key(path, PLATFORM)
    assert not created and again.public_hex == key.public_hex


def test_a_passphrase_seals_the_key(tmp_path):
    path = tmp_path / "plugin-signing-key.json"
    key = signing.create_key(path, PLATFORM, passphrase="correct horse")
    text = path.read_text(encoding="utf-8")
    assert json.loads(text)["passphrase"] is True and key.seed.hex() not in text
    assert signing.load_key(path, "correct horse").seed == key.seed
    with pytest.raises(signing.SigningKeyError, match="not the signing key's"):
        signing.load_key(path, "wrong horse")
    with pytest.raises(signing.SigningKeyError, match="needs its passphrase"):
        signing.load_key(path)


def test_a_key_file_that_does_not_match_is_refused(tmp_path):
    path = tmp_path / "plugin-signing-key.json"
    signing.create_key(path, PLATFORM)
    record = json.loads(path.read_text(encoding="utf-8"))
    record["public"] = RFC_PUBLIC
    path.write_text(json.dumps(record), encoding="utf-8")
    with pytest.raises(signing.SigningKeyError, match="does not match"):
        signing.load_key(path)
    path.write_text("{}", encoding="utf-8")
    with pytest.raises(signing.SigningKeyError, match="not a signing key file"):
        signing.load_key(path)


def test_the_default_key_path_is_outside_the_install(tmp_path):
    assert signing.default_key_path("windows", {"APPDATA": str(tmp_path)}) == tmp_path / "M-Strata" / "plugin-signing-key.json"
    assert signing.default_key_path("linux", {"XDG_CONFIG_HOME": str(tmp_path)}) == tmp_path / "m-strata" / "plugin-signing-key.json"
    assert signing.default_key_path("linux", {}) == Path.home() / ".config" / "m-strata" / "plugin-signing-key.json"


def fixture_files() -> dict[str, bytes]:
    """The C++ verifier's fixtures: one folder's manifest and program, and its signature for each platform, signed with
    RFC 8032's TEST 1 key (deterministic: Ed25519 signs the same statement the same way)."""
    files = {"plugin.json": FIXTURE_MANIFEST.encode("utf-8"), "program": FIXTURE_PROGRAM}
    manifest_sha = signing._sha256_bytes(files["plugin.json"])
    for platform, program in (("windows", "signed-tool.exe"), ("linux", "signed-tool")):
        statement = signing.statement(RFC_PUBLIC, "signedtool", platform, manifest_sha, {program: signing._sha256_bytes(FIXTURE_PROGRAM)})
        files[f"plugin.sig.{platform}"] = signing.envelope(statement, RFC_SEED)
    return files


def test_the_cpp_fixtures_are_this_signers():
    for name, data in fixture_files().items():
        assert (FIXTURES / name).read_bytes() == data, f"{name}: regenerate with --write if the format changed"


if __name__ == "__main__" and "--write" in sys.argv:
    FIXTURES.mkdir(parents=True, exist_ok=True)
    for name, data in fixture_files().items():
        (FIXTURES / name).write_bytes(data)
    print(f"wrote {FIXTURES}")
