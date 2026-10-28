"""dispatch.py - our engine's main dispatcher.

A thin supervisor over the proven per-family backend engines: it maps a requested model to a family, spins up
that family's backend (its own serve/server.py + engine, resident), proxies a single stable front port to it,
and on a model/family switch stops the current backend (freeing VRAM) and brings up the other. qwen runs on the
Strata lineage; glm/deepseek on the Maya lineage. The backends are the untouched proven engines (reviewed, not
reinvented); this dispatcher is the only new piece.

    python dispatch.py --families families.json            # front on the registry's front_port

Front endpoints (proxied to the active backend): the full OpenAI/Anthropic API each backend serves
(/v1/chat/completions, /v1/messages, /v1/models, ...). A request's "model" field selects the family; with no
match the active backend (or the default family) handles it. Plus the dispatcher's own control plane:
  GET  /dispatch/status           - the active family, the known families, backend health
  POST /dispatch/switch {"family":"glm"}  - pre-warm a family (swaps the backend)
"""
from __future__ import annotations

import argparse
import http.client
import json
import os
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HOP_BY_HOP = {"connection", "keep-alive", "proxy-authenticate", "proxy-authorization", "te", "trailers",
              "transfer-encoding", "upgrade", "content-length", "host"}


class Backend:
    """One family's resident engine process (its serve/server.py), launched on demand."""

    def __init__(self, key: str, spec: dict):
        self.key = key
        self.spec = spec
        self.port = int(spec["port"])
        self.proc: subprocess.Popen | None = None

    def start(self, log_dir: Path) -> None:
        env = dict(os.environ)
        cfg = {}
        try:
            cfg = json.loads(Path(self.spec["config"]).read_text(encoding="utf-8"))
        except Exception:
            pass
        for d in cfg.get("lib_dirs", []):          # CUDA bins etc., as the family's own launcher sets them
            env["PATH"] = d + os.pathsep + env.get("PATH", "")
        cmd = [self.spec["python"], "-m", "serve.server", "--engine", "strata",
               "--config", self.spec["config"], "--port", str(self.port), "--host", "127.0.0.1"]
        log_dir.mkdir(parents=True, exist_ok=True)
        self._log = open(log_dir / f"backend-{self.key}.log", "ab", buffering=0)
        print(f"[dispatch] starting backend '{self.key}' ({self.spec['lineage']}) on :{self.port}", flush=True)
        self.proc = subprocess.Popen(cmd, cwd=self.spec["cwd"], env=env, stdout=self._log, stderr=self._log,
                                     stdin=subprocess.DEVNULL)

    def stop(self) -> None:
        if self.proc is None:
            return
        print(f"[dispatch] stopping backend '{self.key}' (pid {self.proc.pid}) to free the GPU", flush=True)
        try:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=30)
        except Exception as e:
            print(f"[dispatch] stop error for '{self.key}': {e}", flush=True)
        finally:
            try:
                self._log.close()
            except Exception:
                pass
            self.proc = None

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def healthy(self, timeout: float = 2.0) -> bool:
        try:
            c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
            c.request("GET", "/health")
            r = c.getresponse()
            r.read()
            c.close()
            return r.status == 200
        except Exception:
            return False

    def wait_ready(self, overall_timeout: float = 1800.0) -> bool:
        """A cold model load (tens of GB + the expert arena) can take minutes; poll /health until it answers,
        but give up if the process dies."""
        t0 = time.time()
        while time.time() - t0 < overall_timeout:
            if not self.alive():
                print(f"[dispatch] backend '{self.key}' exited during load (see its log)", flush=True)
                return False
            if self.healthy():
                print(f"[dispatch] backend '{self.key}' ready after {time.time() - t0:.0f}s", flush=True)
                return True
            time.sleep(1.0)
        return False


class Dispatcher:
    def __init__(self, registry: dict, log_dir: Path):
        self.families = registry["families"]
        self.default_family = registry.get("default_family") or next(iter(self.families))
        self.log_dir = log_dir
        self.active: Backend | None = None
        self.lock = threading.Lock()

    def family_for_model(self, model: str | None) -> str:
        if model:
            m = model.strip().lower()
            for key, spec in self.families.items():
                if m == key or m == spec.get("model_name", "").lower() or m in [a.lower() for a in spec.get("aliases", [])]:
                    return key
            for key, spec in self.families.items():      # loose contains-match (e.g. "...glm...")
                if key in m or any(a.lower() in m for a in spec.get("aliases", [])):
                    return key
        if self.active is not None:
            return self.active.key
        return self.default_family

    def ensure(self, family: str) -> Backend:
        """Guarantee `family`'s backend is the resident one, swapping if a different family is up."""
        if family not in self.families:
            raise KeyError(family)
        with self.lock:
            if self.active is not None and self.active.key == family and self.active.alive():
                return self.active
            if self.active is not None:
                self.active.stop()
                self.active = None
            b = Backend(family, self.families[family])
            b.start(self.log_dir)
            if not b.wait_ready():
                b.stop()
                raise RuntimeError(f"backend '{family}' failed to become ready")
            self.active = b
            return b

    def status(self) -> dict:
        a = self.active
        return {
            "active_family": a.key if a else None,
            "active_healthy": bool(a and a.healthy()),
            "default_family": self.default_family,
            "families": {k: {"model_name": v.get("model_name"), "lineage": v.get("lineage"), "port": v.get("port")}
                         for k, v in self.families.items()},
        }

    def stop_all(self) -> None:
        with self.lock:
            if self.active is not None:
                self.active.stop()
                self.active = None


def _make_handler(disp: Dispatcher):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # quieter
            pass

        def _body(self) -> bytes:
            n = int(self.headers.get("Content-Length", 0) or 0)
            return self.rfile.read(n) if n else b""

        def _json(self, code: int, obj: dict):
            data = json.dumps(obj).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _route_control(self, body: bytes) -> bool:
            if self.path == "/dispatch/status":
                self._json(200, disp.status())
                return True
            if self.path == "/dispatch/switch":
                try:
                    fam = json.loads(body or b"{}").get("family")
                    disp.ensure(fam)
                    self._json(200, {"switched_to": fam, "status": disp.status()})
                except Exception as e:
                    self._json(400, {"error": str(e)})
                return True
            return False

        def _proxy(self, body: bytes):
            model = None
            if body and "application/json" in (self.headers.get("Content-Type", "")):
                try:
                    model = json.loads(body).get("model")
                except Exception:
                    model = None
            fam = disp.family_for_model(model)
            try:
                backend = disp.ensure(fam)
            except Exception as e:
                self._json(503, {"error": f"backend '{fam}' not available: {e}"})
                return
            try:
                c = http.client.HTTPConnection("127.0.0.1", backend.port, timeout=600)
                fwd = {k: v for k, v in self.headers.items() if k.lower() not in HOP_BY_HOP}
                fwd["Host"] = f"127.0.0.1:{backend.port}"
                c.request(self.command, self.path, body=body or None, headers=fwd)
                r = c.getresponse()
                self.send_response(r.status)
                streaming = False
                for k, v in r.getheaders():
                    if k.lower() in HOP_BY_HOP:
                        continue
                    if k.lower() == "content-type" and "event-stream" in v.lower():
                        streaming = True
                    self.send_header(k, v)
                if streaming:
                    self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                while True:
                    chunk = r.read(65536)
                    if not chunk:
                        break
                    if streaming:
                        self.wfile.write(f"{len(chunk):X}\r\n".encode() + chunk + b"\r\n")
                        self.wfile.flush()
                    else:
                        self.wfile.write(chunk)
                if streaming:
                    self.wfile.write(b"0\r\n\r\n")
                c.close()
            except Exception as e:
                try:
                    self._json(502, {"error": f"proxy to '{fam}' failed: {e}"})
                except Exception:
                    pass

        def do_GET(self):
            if self._route_control(b""):
                return
            self._proxy(b"")

        def do_POST(self):
            body = self._body()
            if self._route_control(body):
                return
            self._proxy(body)

    return Handler


def main():
    ap = argparse.ArgumentParser(description="our engine's family dispatcher")
    ap.add_argument("--families", default=str(Path(__file__).with_name("families.json")))
    ap.add_argument("--port", type=int, default=None, help="front port (default: the registry's front_port)")
    ap.add_argument("--host", default="127.0.0.1",
                    help="front bind address. Default 127.0.0.1 (loopback only). 0.0.0.0 exposes the API AND the "
                         "unauthenticated /dispatch control plane to the whole LAN - only do that behind a firewall.")
    ap.add_argument("--prewarm", default=None, help="a family to spin up at start (default: none; lazy on first request)")
    a = ap.parse_args()
    registry = json.loads(Path(a.families).read_text(encoding="utf-8"))
    front_port = a.port or int(registry.get("front_port", 8080))
    log_dir = Path(__file__).with_name("logs")
    disp = Dispatcher(registry, log_dir)
    if a.prewarm:
        disp.ensure(a.prewarm)
    if a.host == "0.0.0.0":
        print("[dispatch] WARNING: front bound to 0.0.0.0 - the API and the unauthenticated /dispatch control "
              "plane are reachable from the LAN. Firewall the port or front it with auth.", flush=True)
    srv = ThreadingHTTPServer((a.host, front_port), _make_handler(disp))
    print(f"[dispatch] front on {a.host}:{front_port}; families={list(disp.families)}; default={disp.default_family}", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        disp.stop_all()


if __name__ == "__main__":
    main()
