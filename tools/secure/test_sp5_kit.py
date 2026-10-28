"""SP5 M2 Task 9: the acceptance kit's corpus mode on strata-api-probe.

The GPU parity run itself happens on the owner's box (docs/validation/sp5-parity-kit.md);
this suite proves the KIT MACHINERY on the dev host: a fixture corpus (one text, one
shape, one divergent-by-decision case) against canned loopback responses lands exactly
one `case,class,RESULT,detail` line per case with the class recorded, the command lines
and engine versions ride the header, --dry-run renders the exact owner-box commands, and
a missing results file is a FAIL, never a skip.
"""

from __future__ import annotations

import http.server
import json
import os
import subprocess
import threading
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCHEMA = "case,class,RESULT,detail"


def probe_binary() -> Path:
    from_env = os.environ.get("STRATA_API_PROBE")
    if from_env:
        path = Path(from_env)
        if path.is_file():
            return path
        pytest.fail(f"STRATA_API_PROBE={from_env} is not a file")
    path = REPO / "secure_frontend/out/build/linux-gcc/strata-api-probe"
    if path.is_file():
        return path
    pytest.skip("strata-api-probe is not built (linux-gcc preset) — environment skip")


class CannedHandler(http.server.BaseHTTPRequestHandler):
    """Canned chat-completion replies keyed by the prompt marker in the body."""

    routes: dict = {}
    protocol_version = "HTTP/1.1"  # the lane client accepts only HTTP/1.1 response lines (by design)

    def do_POST(self) -> None:  # noqa: N802 — the http.server name
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8")
        reply, status = ({"content": "no canned reply", "finish": "stop"}, 200)
        for marker, canned in self.routes.items():
            if marker in body:
                reply, status = canned, canned.get("status", 200)
                break
        if status != 200:
            payload = json.dumps({"error": {"message": "canned error", "type": "invalid_request_error"}})
        else:
            payload = json.dumps(
                {
                    "choices": [{"message": {"role": "assistant", "content": reply["content"]},
                                 "finish_reason": reply["finish"]}],
                    "usage": {"prompt_tokens": 7, "completion_tokens": 5, "total_tokens": 12},
                }
            )
        raw = payload.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")  # the client reads to EOF for a complete response
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *args) -> None:  # silence the server log
        return


@pytest.fixture()
def canned_server():
    routes = {
        "hello-marker": {"content": "Hi there!", "finish": "stop"},
        "one-marker": {"content": "One", "finish": "length"},
        "absent-marker": {"status": 404, "content": "", "finish": "stop"},
    }

    class Routed(CannedHandler):
        pass

    Routed.routes = routes
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Routed)
    port = server.server_address[1]
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield port
    finally:
        server.shutdown()
        server.server_close()


@pytest.fixture()
def kit_inputs(tmp_path: Path):
    token = tmp_path / "token"
    token.write_text("kit-token\n", encoding="utf-8")
    corpus = tmp_path / "corpus.txt"
    corpus.write_text(
        "# fixture corpus: one case per equivalence class (C-6)\n"
        '{"case":"hello-text","class":"text","request":{"messages":[{"role":"user","content":"hello-marker"}],'
        '"temperature":0,"seed":7,"max_tokens":8}}\n'
        '{"case":"one-shape","class":"shape","expect":{"status":200,"finish":"length"},"request":'
        '{"messages":[{"role":"user","content":"one-marker"}],"max_tokens":1}}\n'
        '{"case":"absent-route","class":"divergent-by-decision","authorised_by":"B6","request":'
        '{"messages":[{"role":"user","content":"absent-marker"}]}}\n',
        encoding="utf-8",
    )
    return token, corpus


def run_probe(args: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run([str(probe_binary())] + args, capture_output=True, text=True, timeout=60)


def result_lines(stdout: str) -> list[list[str]]:
    body = stdout.splitlines()
    assert SCHEMA in body, f"no {SCHEMA!r} schema header in:\n{stdout}"
    rows = []
    for line in body[body.index(SCHEMA) + 1:]:
        parts = line.split(",", 3)
        if len(parts) == 4:
            rows.append(parts)
    return rows


def base_args(port: int, token: Path, corpus: Path) -> list[str]:
    return ["--port", str(port), "--token-file", str(token), "--model", "glm-parity", "--corpus", str(corpus)]


def test_corpus_mode_lands_one_typed_line_per_case(canned_server, kit_inputs, tmp_path):
    probe = probe_binary()
    token, corpus = kit_inputs
    results = tmp_path / "results.csv"
    done = run_probe(
        base_args(canned_server, token, corpus)
        + ["--command-line", "ref-server --serve --context 32768",
           "--command-line", "strata serve --config parity.toml",
           "--engine-version", "strata-engine v0.2.1.3",
           "--engine-version", "MAYA v1.1.21",
           "--results", str(results)]
    )
    assert done.returncode == 0, done.stdout + done.stderr
    written = results.read_text(encoding="utf-8")
    assert written == done.stdout, "the results file must equal what the probe printed"
    header = written.splitlines()
    assert header[0] == "# strata-api-probe sp5-parity results"
    assert header[1] == "# command: ref-server --serve --context 32768"
    assert header[2] == "# command: strata serve --config parity.toml"
    assert header[3] == "# engine-version: strata-engine v0.2.1.3"
    assert header[4] == "# engine-version: MAYA v1.1.21"
    rows = result_lines(written)
    assert [row[:3] for row in rows] == [
        ["hello-text", "text", "PASS"],
        ["one-shape", "shape", "PASS"],
        ["absent-route", "divergent-by-decision", "RECORDED"],
    ], written
    assert "sha256=" in rows[0][3] and "bytes=" in rows[0][3]
    assert "status=200" in rows[1][3] and "finish=length" in rows[1][3] and "usage=yes" in rows[1][3]
    assert "authorised_by=B6" in rows[2][3] and "status=404" in rows[2][3]


def test_shape_expectation_mismatch_is_a_fail_line(canned_server, tmp_path: Path):
    probe = probe_binary()
    token = tmp_path / "token"
    token.write_text("kit-token\n", encoding="utf-8")
    corpus = tmp_path / "corpus.txt"
    # the canned reply finishes "length"; the case expects "stop".
    corpus.write_text(
        '{"case":"one-shape","class":"shape","expect":{"status":200,"finish":"stop"},"request":'
        '{"messages":[{"role":"user","content":"one-marker"}],"max_tokens":1}}\n',
        encoding="utf-8",
    )
    done = run_probe(base_args(canned_server, token, corpus) + ["--results", str(tmp_path / "r.csv")])
    assert done.returncode == 1, done.stdout + done.stderr
    rows = result_lines(done.stdout)
    assert rows == [["one-shape", "shape", "FAIL", "status=200 finish=length usage=yes"]], done.stdout


def test_compare_reports_and_missing_results_file_fails_not_skips(tmp_path: Path):
    probe = probe_binary()
    ref = tmp_path / "ref.csv"
    ours = tmp_path / "ours.csv"
    rows = (
        f"{SCHEMA}\n"
        "hello-text,text,PASS,bytes=9 sha256=" + "a" * 64 + "\n"
        "one-shape,shape,PASS,status=200 finish=length usage=yes\n"
        "absent-route,divergent-by-decision,RECORDED,authorised_by=B6 status=404\n"
    )
    ref.write_text(rows, encoding="utf-8")
    ours.write_text(rows, encoding="utf-8")
    done = run_probe(["--compare", str(ref), str(ours)])
    assert done.returncode == 0, done.stdout + done.stderr
    compared = result_lines(done.stdout)
    assert [row[:3] for row in compared] == [
        ["hello-text", "text", "PASS"],
        ["one-shape", "shape", "PASS"],
        ["absent-route", "divergent-by-decision", "PASS"],
    ], done.stdout
    # A missing results file is a FAIL line with exit 1 — never a silent skip.
    missing = tmp_path / "gone.csv"
    done = run_probe(["--compare", str(ref), str(missing)])
    assert done.returncode == 1
    assert "SKIP" not in done.stdout
    assert f"corpus,compare,FAIL,missing_results_file:{missing}" in done.stdout, done.stdout


def test_dry_run_renders_the_exact_owner_box_commands(tmp_path: Path):
    probe = probe_binary()
    token, corpus = kit_inputs_of(tmp_path)
    done = run_probe(
        ["--dry-run", "--corpus", str(corpus), "--token-file", str(token), "--model", "glm-parity",
         "--ref-port", "8081", "--serve-port", "8080",
         "--ref-results", str(tmp_path / "ref.csv"), "--serve-results", str(tmp_path / "ours.csv"),
         "--command-line", "ref-server --serve --context 32768",
         "--command-line", "strata serve --config parity.toml",
         "--engine-version", "strata-engine v0.2.1.3",
         "--engine-version", "MAYA v1.1.21"]
    )
    assert done.returncode == 0, done.stdout + done.stderr
    lines = done.stdout.splitlines()
    assert f"'{probe}' --port 8081 --token-file '{token}' --model 'glm-parity' --corpus '{corpus}' " \
           f"--command-line 'ref-server --serve --context 32768' --command-line 'strata serve --config parity.toml' " \
           f"--engine-version 'strata-engine v0.2.1.3' --engine-version 'MAYA v1.1.21' --results '{tmp_path / 'ref.csv'}'" in lines, done.stdout
    assert f"'{probe}' --port 8080 --token-file '{token}' --model 'glm-parity' --corpus '{corpus}' " \
           f"--command-line 'ref-server --serve --context 32768' --command-line 'strata serve --config parity.toml' " \
           f"--engine-version 'strata-engine v0.2.1.3' --engine-version 'MAYA v1.1.21' --results '{tmp_path / 'ours.csv'}'" in lines, done.stdout
    assert f"'{probe}' --compare '{tmp_path / 'ref.csv'}' '{tmp_path / 'ours.csv'}'" in lines, done.stdout


def kit_inputs_of(tmp_path: Path):
    token = tmp_path / "token"
    token.write_text("kit-token\n", encoding="utf-8")
    corpus = tmp_path / "corpus.txt"
    corpus.write_text("# dry-run needs only the path\n", encoding="utf-8")
    return token, corpus
