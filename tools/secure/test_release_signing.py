"""SP7 M2 Task 4: the release-side key material and signer (scripts/release/).

Provenance discipline (lane ruling R-T0-b): no hex is ever hand-transcribed.
The official RFC 8032 vectors arrive as the committed fixture JSON (fetched from
rfc-editor.org and re-derived with the RFC's own code in the lane); this suite
asserts the pure-Python signer reproduces those signatures exactly, that the
committed release fixture bytes are reproducible by the signer, and that the
same fixture bytes are what the C++ verifier test already pins verbatim
(secure_frontend_release_manifest_tests.python_signed_fixture_verifies).
No signing key lives in this repo: the fixture seed is the RFC's own public test
vector; CLI tests generate ephemeral keys."""

from __future__ import annotations

import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "scripts" / "release"
FIXTURES = REPO / "secure_frontend" / "tests" / "fixtures" / "release"
CPP_TEST = REPO / "secure_frontend" / "tests" / "release_manifest_test.cpp"


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPTS / f"{name}.py")
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_scripts_exist() -> None:
    assert (SCRIPTS / "keygen.py").is_file(), "scripts/release/keygen.py exists"
    assert (SCRIPTS / "sign.py").is_file(), "scripts/release/sign.py exists"


def test_signer_reproduces_the_official_rfc8032_vectors() -> None:
    sign = _load("sign")
    vectors = json.loads((FIXTURES / "rfc8032-vectors.json").read_text(encoding="utf-8"))
    assert len(vectors) >= 4, "the committed vector fixture holds the RFC's test vectors"
    for key, vector in vectors.items():
        seed = bytes.fromhex(vector["sk"][:64])
        message = bytes.fromhex(vector["msg"])
        signature = sign.sign(seed, message)
        assert signature.hex() == vector["sig"], f"vector {key}: the signer must reproduce the RFC signature"
        assert sign.secret_to_public(seed).hex() == vector["pk"], f"vector {key}: the public key derives identically"
        assert sign.verify(bytes.fromhex(vector["pk"]), message, signature), f"vector {key}: verify accepts"
        tampered = bytearray(signature)
        tampered[0] ^= 0x01
        assert not sign.verify(bytes.fromhex(vector["pk"]), message, bytes(tampered)), f"vector {key}: a forged bit is rejected"


def test_keygen_is_deterministic_and_prints_the_pinned_hex() -> None:
    keygen = _load("keygen")
    vectors = json.loads((FIXTURES / "rfc8032-vectors.json").read_text(encoding="utf-8"))
    seed = vectors["1"]["sk"][:64]
    first = keygen.key_material(seed)
    second = keygen.key_material(seed)
    assert first == second, "the same seed gives the same key material"
    assert first["public_hex"] == vectors["1"]["pk"], "the pinned hex is the RFC vector-1 public key"


def test_fixture_envelope_is_reproducible_by_the_signer() -> None:
    sign = _load("sign")
    vectors = json.loads((FIXTURES / "rfc8032-vectors.json").read_text(encoding="utf-8"))
    seed = bytes.fromhex(vectors["1"]["sk"][:64])
    manifest = (FIXTURES / "manifest.json").read_bytes()
    envelope = (FIXTURES / "signed-envelope.txt").read_bytes()
    assert envelope == sign.envelope(manifest, seed), "the committed envelope is exactly what the signer emits"
    assert (FIXTURES / "pubkey.hex").read_text(encoding="utf-8").strip() == vectors["1"]["pk"], "the published key matches"
    assert sign.verify_envelope(envelope, bytes.fromhex(vectors["1"]["pk"])), "the signer's own verifier accepts the fixture"
    assert not sign.verify_envelope(envelope + b" ", bytes.fromhex(vectors["1"]["pk"])), "a trailing byte breaks verification"


def test_fixture_bytes_are_what_the_cpp_suite_verifies() -> None:
    # The C++ suite pins the good envelope as a raw literal; the Python signer's
    # fixture must be byte-identical — that is the cross-tool interlock.
    source = CPP_TEST.read_text(encoding="utf-8")
    match = re.search(r'kGoodEnvelope = R"ENV\((.*?)\)ENV"', source, re.S)
    assert match, "the generated C++ test still pins the good envelope as R\"ENV(...)ENV\""
    assert (FIXTURES / "signed-envelope.txt").read_bytes() == match.group(1).encode("utf-8"), (
        "the fixture and the C++-pinned envelope are the same bytes"
    )


def test_round_trip_with_an_ephemeral_key(tmp_path: Path) -> None:
    sign = _load("sign")
    keygen = _load("keygen")
    material = keygen.key_material()  # no seed: an ephemeral key
    assert re.fullmatch(r"[0-9a-f]{64}", material["public_hex"]), "the public pin is 64 lowercase hex"
    manifest = b'{"format":"strata-release-v1","version":"0.0.1","channel":"stable","components":[]}'
    envelope = sign.envelope(manifest, bytes.fromhex(material["seed_hex"]))
    assert sign.verify_envelope(envelope, bytes.fromhex(material["public_hex"])), "an ephemeral round trip verifies"
    wrong = bytes([bytes.fromhex(material["public_hex"])[0] ^ 0x01]) + bytes.fromhex(material["public_hex"])[1:]
    assert not sign.verify_envelope(envelope, wrong), "a wrong key refuses"


def test_cli_sign_verify_flow(tmp_path: Path) -> None:
    key_file = tmp_path / "signing-key.hex"
    manifest = tmp_path / "manifest.json"
    envelope = tmp_path / "envelope.txt"
    result = subprocess.run([sys.executable, str(SCRIPTS / "keygen.py"), "--out-key", str(key_file)],
                            capture_output=True, text=True, check=True)
    pin = None
    for line in result.stdout.splitlines():
        if line.startswith("pinned public key:"):
            pin = line.split(":", 1)[1].strip()
    assert pin and re.fullmatch(r"[0-9a-f]{64}", pin), f"keygen prints the pinned hex; got: {result.stdout!r}"
    manifest.write_bytes(b'{"format":"strata-release-v1","version":"9.9.9","channel":"stable","components":[]}')
    subprocess.run([sys.executable, str(SCRIPTS / "sign.py"), "--key-file", str(key_file), "--manifest", str(manifest),
                    "--out", str(envelope)], check=True, capture_output=True)
    verify = subprocess.run([sys.executable, str(SCRIPTS / "sign.py"), "--verify", "--key-hex", pin,
                             "--envelope", str(envelope)], capture_output=True, text=True, check=True)
    assert "verifies" in verify.stdout, verify.stdout
    envelope.write_bytes(envelope.read_bytes() + b"tamper")
    bad = subprocess.run([sys.executable, str(SCRIPTS / "sign.py"), "--verify", "--key-hex", pin, "--envelope",
                          str(envelope)], capture_output=True, text=True)
    assert bad.returncode != 0, "a tampered envelope makes --verify exit non-zero"
