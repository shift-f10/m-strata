"""Exercise the card's actual decimal controls with a small DOM event harness.

Node is test-only; no JavaScript runtime is added to the product.
"""
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_decimal_card_sliders_preserve_values_and_validate_bounds():
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node is needed for the DOM event harness")
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    control = source[source.index("  function numberControl("):source.index("  function choiceControl(")]
    control += source[source.index("  function optionControl("):source.index("  function buildOptions(")]
    harness = r'''
const assert = require("node:assert/strict");
function el(tag, cls) {
  return {tag, className: cls, children: [], attrs: {}, events: {}, value: "",
    appendChild(child) { this.children.push(child); },
    setAttribute(k, v) { this.attrs[k] = v; },
    addEventListener(k, callback) { this.events[k] = callback; },
    focus() {},
    emit(name) { this.events[name](); }};
}
const document = {createTextNode: text => ({text})};
const num = String;
function inputs(control) { return control.node.children.filter(n => n.tag === "input"); }
function check(name, min, max, initial) {
  const c = numberControl({name, label: name, min, max}, initial, false, false);
  const range = inputs(c).find(n => n.type === "range");
  const value = inputs(c).find(n => n.type === "number");
  assert.ok(range, name + " offers a slider");
  assert.ok(value, name + " retains exact decimal entry");
  assert.equal(range.step, "0.01");
  assert.equal(range.id, "opt-" + name);
  assert.equal(value.attrs["aria-label"], name + " value");
  assert.equal(c.read().value, initial);
  range.value = "0.5"; range.emit("input");
  assert.equal(c.read().value, 0.5);
  assert.equal(value.value, "0.5", "dragging updates the visible value");
  value.value = "0.375"; value.emit("input");
  assert.equal(c.read().value, 0.375, "typed precision survives");
  assert.equal(range.attrs["aria-valuetext"], "0.375");
  c.set(initial); assert.equal(c.read().value, initial, "reset preserves default precision");
  value.value = String(max + 1); assert.ok(c.read().error);
  value.value = ""; assert.ok(c.read().error);
  value.value = "0";
  if (min > 0) { assert.ok(c.read().error); assert.ok(Number(range.min) > 0); }
  else { assert.equal(c.read().value, 0); }
}
check("temperature", 0, 2, 0.375);
check("top_p", Number.MIN_VALUE, 1, 0.005);
check("min_p", 0, 1, 0.075);
const integer = numberControl({name: "top_k", min: 1, max: 64}, 20, true, false);
assert.equal(inputs(integer).length, 1, "integer controls keep their existing behavior");
assert.equal(integer.read().value, 20);
const auto = numberControl({name: "share", min: 0, max: 1}, "auto", false, true);
assert.equal(auto.read().value, "auto");
assert.ok(inputs(auto).every(n => n.disabled));
auto.set(0.4); assert.equal(auto.read().value, 0.4);
assert.ok(inputs(auto).every(n => !n.disabled));
const narrow = numberControl({name: "top_p", min: 0.001, max: 0.005}, 0.003, false, false);
const narrowRange = inputs(narrow).find(n => n.type === "range");
assert.ok(Number(narrowRange.min) <= Number(narrowRange.max), "narrowed card bounds remain a valid range");
assert.ok(Number(narrowRange.step) <= 0.004, "a narrow card range is still adjustable");
for (const name of ["temperature", "top_p", "min_p", "spec_min_p"]) {
  const routed = optionControl({name, kind: "decimal", min: 0, max: 1}, 0.375);
  assert.ok(inputs(routed).some(n => n.type === "range"), name + " dispatches to a decimal slider");
  assert.equal(routed.read().value, 0.375);
}
const routedAuto = optionControl({name: "share", kind: "decimal_or_auto", min: 0, max: 1}, "auto");
assert.ok(inputs(routedAuto).some(n => n.type === "range"));
assert.equal(routedAuto.read().value, "auto");
routedAuto.set(0.375); assert.equal(routedAuto.read().value, 0.375);
'''
    result = subprocess.run([node, "-e", control + harness], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr


def test_chat_drawer_does_not_round_an_untouched_sampling_value():
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node is needed for the DOM event harness")
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    control = source[source.index("  function showSettings("):source.index("  // ---------------------------------------------------------------- about")]
    harness = r'''
const assert = require("node:assert/strict");
const nodes = {};
function $(id) {
  if (!nodes[id]) { nodes[id] = {value: "", textContent: "", setAttribute() {},
    getAttribute() { return "true"; }, querySelectorAll() { return []; }, querySelector() { return null; }}; }
  return nodes[id];
}
// Range inputs sanitize their position to a bounded, stepped value. The model's value must
// survive this display operation, even when a different slider is then moved.
function range(id, min, max, step) {
  let position = min;
  Object.defineProperty($(id), "value", {get() { return String(position); }, set(v) {
    position = Math.max(min, Math.min(max, min + Math.round((Number(v) - min) / step) * step));
  }});
}
range("s-temp", 0, 2, 0.01); range("s-topp", 0.01, 1, 0.01);
const state = {};
const DEFAULTS = {thinking: "high"}, LEVELS = {high: "High"}, SLOT_SETTINGS = "settings";
const autoOffered = () => false, searchOffered = () => true;
const runningCard = () => null, cardOption = () => null;
const cardSettings = () => ({temperature: state.settings.temperature, top_p: state.settings.top_p});
function save() {} function toast() {}
function reset(tuned) {
  state.settings = {thinking: "high", temperature: 0.375, top_p: 0.005, tuned, max: "", seed: "", show: true};
  showSettings();
}
reset(false);
assert.ok($("o-temp").textContent.includes("0.375"), "display must not round the card's temperature");
assert.ok($("o-topp").textContent.includes("0.005"), "display must not round the card's top-p");
applySettings();
assert.equal(state.settings.temperature, 0.375);
assert.equal(state.settings.top_p, 0.005);
assert.equal(state.settings.tuned, false);
reset(false); $("s-temp").value = "0.5"; applySettings();
assert.equal(state.settings.temperature, 0.5);
assert.equal(state.settings.top_p, 0.005, "moving temperature must not clamp top-p");
assert.equal(state.settings.tuned, true);
reset(false); $("s-topp").value = "0.7"; applySettings();
assert.equal(state.settings.temperature, 0.375, "moving top-p must not round temperature");
assert.ok(Math.abs(state.settings.top_p - 0.7) < 1e-15);
reset(true); applySettings();
assert.equal(state.settings.temperature, 0.375);
assert.equal(state.settings.top_p, 0.005);
'''
    result = subprocess.run([node, "-e", control + harness], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
