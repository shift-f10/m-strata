"""Exercise speed/accuracy card groups using the actual typed controls."""
from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]


def test_speed_group_warns_and_resets_only_accuracy_tradeoffs():
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node is needed for the DOM event harness")
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    control = source[source.index("  function labelOf("):source.index("  // ---------------------------------------------------------------- template review")]
    harness = r'''
const assert = require("node:assert/strict");
function el(tag, cls, text) {
  const n = {tag, className: cls || "", textContent: text || "", children: [], attrs: {}, events: {}, value: "",
    appendChild(child) { this.children.push(child); child.parent = this; return child; },
    setAttribute(k, v) { this.attrs[k] = v; }, getAttribute(k) { return this.attrs[k]; },
    addEventListener(k, callback) { (this.events[k] ||= []).push(callback); },
    focus() {}, emit(name) { (this.events[name] || []).forEach(f => f()); if (this.parent) this.parent.emit(name); }};
  n.classList = {add(cls) { n.className += " " + cls; }, remove(cls) { n.className = n.className.split(" ").filter(c => c !== cls).join(" "); },
    toggle(cls, yes) { this.remove(cls); if (yes) this.add(cls); }};
  Object.defineProperty(n, "options", {get() { return n.children; }});
  return n;
}
const box = el("div");
const ids = {"model-options": box, "model-tradeoff-warning": el("p")};
["generation", "memory", "speed", "template", "calibration", "files"].forEach(name => {
  ids["model-card-tab-" + name] = el("button");
  if (["template", "calibration", "files"].includes(name)) ids["model-card-panel-" + name] = el("section");
});
const $ = id => ids[id] || all(box).find(n => n.id === id);
const clear = n => { n.children = []; };
const document = {createTextNode: text => el("#text", "", text)};
const num = String, fmt = String, valueText = String, DOT = " · ", hostGpus = [];
const launcher = {};
function showStartError() {}
function all(n) { return [n, ...n.children.flatMap(all)]; }
function text(n) { return n.textContent + n.children.map(text).join(" "); }
function named(name) { return launcher.controls.find(c => c.option.name === name); }
const ordinary = {name: "temperature", label: "Temperature", kind: "decimal", min: 0, max: 2, default: .6, value: .375};
const flags = [
  {name: "resident", kind: "boolean", default: false, value: true, accuracy_default: false},
  {name: "skip", kind: "decimal", min: 0, max: 1, default: 0, value: .125, accuracy_default: 0},
  {name: "keep", kind: "integer", min: 1, max: 8, default: 8, value: 8, accuracy_default: 8},
  {name: "mode", kind: "choice", choices: ["exact", "fast"], default: "exact", value: "exact", accuracy_default: "exact"}
].map(o => ({...o, accuracy_tradeoff: true, help: "Can change generated results."}));
buildOptions({name: "one", options: [flags[0], ordinary, ...flags.slice(1)]});
const group = $("model-card-panel-speed").children.find(n => n.tag === "details");
assert.ok(group, "tradeoffs have their own expandable section");
assert.match(text(group), /Speed improvements/);
assert.match(text(group), /May reduce accuracy/);
assert.ok(!group.open, "speed settings start collapsed");
assert.ok(group.children.some(n => n.tag === "summary"));
assert.equal(named("temperature").field.parent, $("model-card-panel-generation"), "sampling stays in Generation, outside tradeoffs");
assert.ok(all(group).includes(named("skip").field));
assert.ok(all(named("skip").field).some(n => n.type === "range"), "fractional speed values use typed sliders");
assert.equal(named("skip").read().value, .125, "existing precise tradeoff value survives rendering");
const summary = group.children.find(n => n.tag === "summary");
assert.match(text(summary), /2 active/);
const reset = all(group).find(n => n.tag === "button" && /accuracy defaults/i.test(text(n)));
assert.ok(reset);
reset.emit("click");
assert.equal(named("temperature").read().value, .375, "group reset preserves other precise settings");
assert.equal(named("resident").read().value, false);
assert.equal(named("skip").read().value, 0);
assert.match(text(summary), /Accuracy defaults/);
const slider = all(named("skip").field).find(n => n.type === "range");
slider.value = ".25"; slider.emit("input");
assert.match(text(summary), /1 active/);
assert.match(text(named("skip").field), /May reduce accuracy/);
const toggle = all(named("resident").field).find(n => n.attrs.role === "switch");
toggle.emit("click");
assert.match(text(summary), /2 active/);
const select = all(named("mode").field).find(n => n.tag === "select");
select.value = "fast"; select.emit("change");
assert.match(text(summary), /3 active/);
assert.deepEqual(collectOptions().values, {resident: true, temperature: .375, skip: .25, keep: 8, mode: "fast"});
resetOptions(); assert.match(text(summary), /Accuracy defaults/);
assert.equal(named("temperature").read().value, .6, "existing full reset still works");
buildOptions({name: "two", options: [ordinary]});
assert.ok(!all(box).some(n => n.tag === "details"), "cards without tradeoffs do not show an empty group");
buildOptions({name: "three", options: [{...flags[0], value: undefined, default: true}]});
assert.equal(named("resident").read().value, false, "absent value starts with accuracy default");
'''
    result = subprocess.run([node, "-e", control + harness],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
