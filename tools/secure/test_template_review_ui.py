"""Behavior of the card's template review: literal source and explicit approval."""
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_template_tab_loads_and_shows_saved_choice_before_review():
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    html = (ROOT / "secure_frontend/web/index.html").read_text(encoding="utf-8")
    tab = source[source.index("  function showCardTab("):source.index("  function initCardTabs(")]
    assert 'inspectCardTemplate()' in tab
    panel = html[html.index('id="model-card-panel-template"'):html.index('id="model-card-panel-calibration"')]
    assert '<details class="model-template"' not in panel
    assert panel.index('id="model-template-status"') < panel.index('id="model-template-mode"')
    assert panel.index('id="model-template-mode"') < panel.index('id="model-template-source"')


def test_template_review_uses_literal_source_and_requires_hash_acknowledgement():
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node is needed for the DOM event harness")
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    assert "  // ---------------------------------------------------------------- template review" in source
    control = source[source.index("  // ---------------------------------------------------------------- template review"):
                     source.index("  // ---------------------------------------------------------------- model template review end")]
    harness = r'''
const assert = require("node:assert/strict");
const nodes = {};
function el(tag, className, text) {
  return {tag, className: className || "", textContent: text || "", children: [], value: "", checked: false,
    appendChild(n) { this.children.push(n); }, setAttribute(k, v) { this[k] = v; }};
}
function $(id) { return nodes[id] || (nodes[id] = el("div")); }
function clear(n) { n.children = []; }
const launcher = {selected: "model-a"};
const findCard = name => ({name});
const replies = [], calls = [];
const jsonPost = value => ({body: JSON.stringify(value)});
const api = (path, request) => { calls.push({path, body: JSON.parse(request.body)}); return replies.shift(); };
const downloadsAnswer = async answer => answer;
const sample = {available: true, mode: "frontend", sha256: "a".repeat(64),
  source: "Hello\n<script>alert(1)</script>\n{{ unsafe.attribute }}",
  risks: [{line: 2, end_line: 3, code: "suspicious", message: "Review this expression"}]};
(async function () {
  resetTemplateReview("model-a");
  replies.push({body: sample}); await inspectCardTemplate();
  const lines = $("model-template-source").children;
  assert.equal(lines.length, 3);
  assert.equal(lines[1].children[1].textContent, "<script>alert(1)</script>", "source is literal text");
  assert.ok(lines[1].className.includes("template-line--risk"));
  assert.ok(lines[2].className.includes("template-line--risk"));
  assert.equal($("model-template-mode").value, "frontend");
  $("model-template-mode").value = "embedded"; updateTemplateSave();
  assert.equal($("model-template-source-review").hidden, true, "source stays hidden until requested");
  assert.equal($("model-template-ack").disabled, true, "new choices require source review");
  assert.equal($("model-template-save").disabled, true);
  await saveCardTemplate(); assert.equal(calls.length, 1, "no approval means no save request");
  templateReview.viewing = true; $("model-template-ack").checked = true; updateTemplateSave();
  assert.equal($("model-template-source-review").hidden, false);
  assert.equal($("model-template-save").disabled, false, "warnings do not prevent explicit approval");
  replies.push({body: {}}, {body: {...sample, mode: "embedded"}});
  await saveCardTemplate();
  assert.deepEqual(calls[1].body, {name: "model-a", mode: "embedded", sha256: sample.sha256, acknowledge: true});
  assert.equal($("model-template-mode").value, "embedded");
  assert.equal($("model-template-ack").checked, true, "saved approval remains visible after the review refresh");
  assert.equal($("model-template-save").disabled, true, "saved unchanged choice does not look unsaved");
  assert.equal($("model-template-save").textContent, "Saved");
  assert.ok($("model-template-status").textContent.includes("next model start"));
  assert.ok($("model-template-status").textContent.includes("Restart"));
  const savedCalls = calls.length;
  await saveCardTemplate(); assert.equal(calls.length, savedCalls, "unchanged saved choice is not submitted again");
  $("model-template-mode").value = "frontend"; changeTemplateMode();
  assert.equal($("model-template-save").disabled, false);
  $("model-template-mode").value = "embedded"; changeTemplateMode();
  assert.equal($("model-template-ack").checked, true, "returning to the saved choice restores its approval indicator");
  $("model-template-mode").value = "frontend"; $("model-template-ack").checked = false;
  replies.push({body: {}}, {body: sample}); await saveCardTemplate();
  assert.deepEqual(calls[3].body, {name: "model-a", mode: "frontend"});
  resetTemplateReview("model-b");
  assert.equal(templateReview.data, null, "changing cards cannot reuse a prior hash");
  assert.equal($("model-template-ack").checked, false);
  renderTemplateReview({...sample, available: false});
  $("model-template-mode").value = "embedded"; $("model-template-ack").checked = true; updateTemplateSave();
  assert.equal($("model-template-save").disabled, true, "unavailable templates cannot be approved");
  launcher.selected = "model-a"; resetTemplateReview("model-a");
  let finish;
  replies.push(new Promise(resolve => { finish = resolve; }));
  const pending = inspectCardTemplate();
  launcher.selected = "model-b"; resetTemplateReview("model-b");
  finish({body: sample}); await pending;
  assert.equal(templateReview.data, null, "an old response cannot populate another card's review");
  const beforeRestore = calls.length;
  replies.push({body: {}}, {error: "The template cannot be read"});
  await saveCardTemplate("frontend");
  assert.deepEqual(calls[beforeRestore].body, {name: "model-b", mode: "frontend"}, "frontend recovery needs no successful review");
  assert.ok($("model-template-status").textContent.includes("next model start"));
  renderTemplateReview({...sample, mode: "embedded", needs_review: true});
  assert.equal($("model-template-ack").checked, false, "changed source requires a new explicit approval");
  assert.ok($("model-template-error").textContent.includes("changed"));
  renderTemplateReview({...sample, source: "prefix\u202Ehidden"});
  assert.equal($("model-template-source").children[0].children[1].textContent, "prefix[U+202E]hidden");
})().catch(e => { console.error(e); process.exitCode = 1; });
'''
    result = subprocess.run([node, "-e", control + harness], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
