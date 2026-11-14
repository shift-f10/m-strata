"""Exercise code-copy reachability in a real browser with the shipped styles."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_code_copy_follows_scroll_and_stays_inside_its_block():
    browser = os.environ.get("STRATA_TEST_BROWSER") or shutil.which("chromium") or shutil.which("google-chrome")
    if not browser and os.name == "nt":
        edge = Path(os.environ.get("PROGRAMFILES(X86)", "")) / "Microsoft/Edge/Application/msedge.exe"
        if edge.is_file():
            browser = str(edge)
    if not browser:
        pytest.skip("A Chromium browser is required for the code-copy layout check")
    web = ROOT / "secure_frontend/web"
    source = (web / "app.js").read_text(encoding="utf-8")
    render = source[source.index("  function codeBlock("):source.index("  function tableRow(")]
    styles = "\n".join((web / name).read_text(encoding="utf-8") for name in ("tokens.css", "components.css", "app.css"))
    harness = r'''
function el(tag, cls, text) { const n = document.createElement(tag); if (cls) n.className = cls; if (text !== undefined) n.textContent = text; return n; }
let copied;
const copyText = text => { copied = text; };
function iconButton(icon, label, action) { const b = el("button", "st-btn st-btn--icon", "Copy"); b.setAttribute("aria-label", label); b.addEventListener("click", action); return b; }
function check(ok, message) { if (!ok) throw new Error(message); }
try {
  const scroll = document.querySelector(".chat-scroll"), bubble = document.querySelector(".st-bubble");
  const code = Array.from({length: 180}, (_, i) => i + " <tag> & " + "long code line ".repeat(15)).join("\n");
  const first = codeBlock("text", code), second = codeBlock("text", "second block\n".repeat(100));
  bubble.append(first, el("p", null, "Between blocks"), second);
  const tail = el("div"); tail.style.height = "1000px"; bubble.append(tail);
  for (const width of [390, 1000]) {
    scroll.style.width = width + "px";
    scroll.scrollTop = 0;
    const top = scroll.getBoundingClientRect().top;
    scroll.scrollTop += first.getBoundingClientRect().top - top + 500;
    const head = first.querySelector(".st-code__head"), button = head.querySelector("button");
    check(Math.abs(head.getBoundingClientRect().top - top) < 2, "Copy header must follow chat scrolling at width " + width);
    check(button.getBoundingClientRect().bottom <= scroll.getBoundingClientRect().bottom, "Copy button must remain visible");
    const pre = first.querySelector("pre"); pre.scrollLeft = 250;
    check(pre.scrollLeft > 0, "Long code must still scroll horizontally");
    check(button.getBoundingClientRect().right <= scroll.getBoundingClientRect().right + 1, "Horizontal code scrolling must not hide copy");
    button.click(); check(copied === code, "Copy must preserve the entire original code");
    scroll.scrollTop += second.getBoundingClientRect().top - top + 300;
    check(head.getBoundingClientRect().bottom <= top + 1, "Previous copy header must leave with its block");
    check(Math.abs(second.querySelector(".st-code__head").getBoundingClientRect().top - top) < 2, "Next block must have its own sticky copy header");
  }
  document.documentElement.setAttribute("data-test", "passed");
} catch (e) { document.documentElement.setAttribute("data-test", "failed: " + e.message); }
'''
    with tempfile.TemporaryDirectory(prefix="strata-code-copy-") as tmp:
        fixture = Path(tmp) / "fixture.html"
        fixture.write_text('<!doctype html><html><head><meta charset="utf-8"><style>' + styles +
                           '</style></head><body><div class="chat-scroll"><div class="chat"><div class="st-msg st-msg--assistant"><div class="st-bubble"></div></div></div></div><script>' +
                           render + harness + '</script></body></html>', encoding="utf-8")
        result = subprocess.run([browser, "--headless", "--no-first-run", "--disable-background-networking",
                                 "--user-data-dir=" + str(Path(tmp) / "profile"), "--window-size=1100,800",
                                 "--dump-dom", fixture.as_uri()], capture_output=True, text=True,
                                encoding="utf-8", timeout=45)
        status = re.search(r'data-test="([^"]*)"', result.stdout)
        assert result.returncode == 0 and status and status[1] == "passed", status[1] if status else "Browser layout check did not complete"
