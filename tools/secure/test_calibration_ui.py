"""Manual calibration requests and reports use explicit bounded inputs."""
from pathlib import Path
from html.parser import HTMLParser
import re
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_calibration_defaults_to_quick_and_preserves_explicit_and_automatic_requests():
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node is needed for the DOM event harness")
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    control = source[source.index("  var CAL_STATES ="):source.index("  async function setAutoRecalibrate(")]
    harness = r'''
const assert = require("node:assert/strict");
const nodes = {};
function el(tag, className, text) {
  return {tag, className: className || "", textContent: text || "", children: [], value: "", checked: false, style: {},
    appendChild(n) { this.children.push(n); return n; }, setAttribute(k, v) { this[k] = v; },
    addEventListener(k, f) { this[k] = f; }};
}
function $(id) { return nodes[id] || (nodes[id] = el("div")); }
function clear(n) { n.children = []; }
function text(n) { return n.textContent + n.children.map(text).join(" "); }
function all(n) { return [n, ...n.children.flatMap(all)]; }
const fmt = n => String(n), num = String, DOT = " / ";
const card = {name: "test-model", calibration: {state: "fresh", before: 10, after: 12}};
const launcher = {selected: card.name, formFor: card.name, acting: false, data: {running: "", state: "idle"}};
const findCard = name => name === card.name ? card : null;
const launcherStarting = () => false;
const collectOptions = () => ({values: {temperature: .375}});
const errors = [], calls = [], notices = [];
function showStartError(message) { errors.push(message); }
const window = {confirm: () => true};
const jsonPost = body => ({body});
const api = async (path, request) => { calls.push({path, body: request.body}); return {}; };
const downloadsAnswer = async promise => promise;
const refreshLauncher = async () => {}, pollLauncher = async () => {}, stopLauncherPoll = () => {};
const renderDetail = () => {};
const toast = (...args) => notices.push(args.join(" "));
const startModel = () => { throw new Error("restore must not restart model"); };
(async function () {
  renderCalibration(card, false);
  const quickDefault = {mode: "quick", workload: "chat", budget_seconds: 1800};
  assert.deepEqual(readCalibrationBenchmark().value, quickDefault, "Calibrate defaults to Quick without opening options");
  assert.equal($("calibration-manual-fields").disabled, true);
  await calibrateModel(false);
  assert.deepEqual(calls.at(-1), {path: "/v1/strata/models/calibrate", body: {name: card.name, options: {temperature: .375}, benchmark: quickDefault}});
  $("calibration-manual-enabled").checked = true;
  $("calibration-mode").value = "thorough";
  $("calibration-workload").value = "custom";
  $("calibration-budget").value = "2.5";
  $("calibration-label").value = "My run";
  $("calibration-custom").value = "Explicit prompt <script> literal";
  $("calibration-resume").checked = true;
  $("calibration-memory").checked = true;
  $("calibration-compare").checked = true;
  let read = readCalibrationBenchmark();
  assert.equal(read.value.budget_seconds, 150);
  assert.equal(read.value.custom_prompt, "Explicit prompt <script> literal");
  assert.equal(read.value.compare_speed, true);
  renderCalibration(card, false);
  assert.equal($("calibration-custom").value, read.value.custom_prompt, "poll preserves explicit custom input");
  await calibrateModel(true);
  assert.deepEqual(calls.at(-1), {path: "/v1/strata/models/calibrate", body: {name: card.name, options: {temperature: .375}, then_start: true}});
  assert.ok(!("benchmark" in calls.at(-1).body), "calibrate-then-start retains legacy request even with manual drafts");
  assert.equal(calls.at(-1).body.then_start, true);
  await calibrateModel(false);
  assert.deepEqual(calls.at(-1).body.benchmark, read.value);
  $("calibration-workload").value = "tools";
  assert.ok(!("custom_prompt" in readCalibrationBenchmark().value), "other workloads never send custom input");
  $("calibration-workload").value = "custom";
  $("calibration-custom").value = "\u00e9".repeat(8193);
  assert.ok(readCalibrationBenchmark().error, "custom limit is UTF-8 bytes");
  $("calibration-custom").value = "";
  assert.ok(readCalibrationBenchmark().error, "empty custom workload rejected");
  $("calibration-workload").value = "chat";
  $("calibration-label").value = "x".repeat(65);
  assert.ok(readCalibrationBenchmark().error, "label length enforced");
  $("calibration-label").value = "R\u00e9sultat";
  assert.ok(readCalibrationBenchmark().error, "label ASCII contract matches backend");
  $("calibration-label").value = "";
  $("calibration-budget").value = "0";
  assert.ok(readCalibrationBenchmark().error);
  $("calibration-budget").value = "1441";
  assert.ok(readCalibrationBenchmark().error);
  $("calibration-budget").value = "30";
  card.calibration.experiment = {
    mode: "quick", workload: "tools", elapsed_s: 30, remaining_s: null, completed: 2, estimated_trials: 5, paused: true,
    outcome: "not_applied", error: "Saved checkpoint no longer matches", noisy: true, variation_pct: 12,
    before: 10, after: 12, settings: ["--threads", "6"],
    notes: ["Checkpoint available"], summary: {decode_tok_s: 12, prompt_tok_s: null, ttft_ms: 0, load_ms: 200,
      cold_ttft_ms: 100, warm_ttft_ms: 10, warm_decode_tok_s: 13, sustained_decode_tok_s: 11,
      variation_pct: 3, quality_pass: false, memory: {ram_free_bytes: null, vram_free_bytes: 1024, context_tokens: 512, target_context_tokens: 4096}},
    skipped: [{setting: "threads", reason: "fixed by card"}],
    trials: [{number: 2, stage: "repeat", status: "contaminated", reason: "competing GPU activity", warnings: ["thermal signal unavailable"], settings: ["threads=4"]}],
    comparison: [{label: "Fast math", status: "failed", reason: "quality gate", before: 10, after: 13, summary: {quality_pass: false, settings: ["option:speed_fast_math", "true"]}}],
    history: [{id: "old", label: "Prior <script> run", date: "2026-11-11", before: 10, after: 12, settings: ["--threads", "4"], applicable: true},
      {id: "stale", label: "Stale run", applicable: false, reason: "engine changed"}]
  };
  renderCalibration(card, false);
  const report = $("model-calibration-experiment");
  assert.match(text(report), /Unavailable/);
  assert.match(text(report), /2.*5/);
  assert.match(text(report), /Paused/);
  assert.match(text(report), /competing GPU activity/);
  assert.match(text(report), /thermal signal unavailable/);
  assert.match(text(report), /quality gate/);
  assert.match(text(report), /Not applied/);
  assert.match(text(report), /Saved checkpoint no longer matches/);
  assert.match(text(report), /Noisy/);
  assert.match(text(report), /12 %/);
  assert.match(text(report), /option:speed_fast_math/);
  assert.match(text(report), /--threads\n4/);
  assert.match(text(report), /0 ms/, "valid zero measurement is distinct from unavailable");
  assert.match(text(report), /Cold time to first token/); assert.match(text(report), /100 ms/);
  assert.match(text(report), /Warm decode/); assert.match(text(report), /13 tok\/s/);
  assert.match(text(report), /Sustained decode/); assert.match(text(report), /11 tok\/s/);
  assert.match(text(report), /fixed by card/);
  assert.match(text(report), /Before/); assert.match(text(report), /After/);
  const trialDetails = all(report).find(n => n.tag === "details" && text(n.children[0]).startsWith("Trial details"));
  trialDetails.open = true; trialDetails.toggle();
  renderCalibration(card, false);
  assert.equal(all(report).find(n => n.tag === "details" && text(n.children[0]).startsWith("Trial details")).open, true,
    "polling does not close expanded trial details");
  const buttons = all(report).filter(n => n.tag === "button");
  assert.equal(buttons.length, 2);
  assert.equal(buttons[1].disabled, true, "stale history cannot restore");
  await buttons[0].click();
  assert.deepEqual(calls.at(-1).body, {name: card.name, restore: "old"});
  assert.equal(launcher.formFor, "", "restore reloads server-selected options before next start");
  assert.ok(notices.some(n => /restart/i.test(n)), "restore explicitly reports no automatic restart");
  const count = calls.length;
  card.calibration.experiment.history[0].applicable = false;
  await buttons[0].click();
  assert.equal(calls.length, count, "a stale rendered restore button rechecks latest applicability");
  assert.ok(all(report).some(n => n.textContent.includes("Prior <script> run")), "labels are literal text nodes");
  card.calibration.experiment = {history: card.calibration.experiment.history};
  renderCalibration(card, false);
  assert.doesNotMatch(text(report), /Unknown mode/, "legacy history alone is not presented as an unavailable benchmark");
  renderCalibrationExperiment(card, {outcome: "not_applied", mode: "quick", workload: "chat",
    summary: {status: "failed", reason: "Benchmark request rejected: max_tokens"}});
  assert.ok(report.children.some(n => n.textContent === "Trial stopped: Benchmark request rejected: max_tokens"),
    "the failure reason is visible without expanding trial details");
  renderCalibration({name: "another", calibration: {state: "missing"}}, false);
  assert.equal($("calibration-custom").value, "", "switching card clears custom input");
  assert.equal($("calibration-manual-enabled").checked, false);
})().catch(error => { console.error(error); process.exitCode = 1; });
'''
    result = subprocess.run([node], input=control + harness, capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr


def test_calibration_html_has_all_controls_and_accessible_labels():
    class Controls(HTMLParser):
        def __init__(self):
            super().__init__()
            self.ids = {}
            self.labels = set()

        def handle_starttag(self, tag, attrs):
            values = dict(attrs)
            if "id" in values:
                assert values["id"] not in self.ids, "duplicate DOM id"
                self.ids[values["id"]] = (tag, values)
            if tag == "label" and "for" in values:
                self.labels.add(values["for"])

    html = Controls()
    html.feed((ROOT / "secure_frontend/web/index.html").read_text(encoding="utf-8"))
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    ids = set(re.findall(r'\$\("(calibration-(?:manual|mode|workload|budget|label|custom|resume|memory|compare)[^"]*)"\)', source))
    assert ids, "manual benchmark controls exist"
    assert ids <= html.ids.keys(), "every referenced manual control is present in the real page"
    for name in ("mode", "workload", "budget", "label", "custom"):
        assert "calibration-" + name in html.labels
    assert html.ids["calibration-custom"][1]["maxlength"] == "16384"
    assert "model-calibration-experiment" in html.ids
