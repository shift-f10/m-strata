"""Card navigation must preserve the typed form and expose hidden errors."""
from html.parser import HTMLParser
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_card_tab_structure_and_launcher_actions():
    class Tree(HTMLParser):
        def __init__(self):
            super().__init__()
            self.stack, self.nodes = [], {}

        def handle_starttag(self, tag, attrs):
            attrs = dict(attrs)
            if attrs.get("id"):
                assert attrs["id"] not in self.nodes, "duplicate ID"
                self.nodes[attrs["id"]] = (attrs, list(self.stack))
            if tag not in {"input", "br", "hr", "img", "meta", "link", "use", "path"}:
                self.stack.append(attrs.get("id", tag))

        def handle_endtag(self, tag):
            if tag not in {"use", "path"}:
                self.stack.pop()

    tree = Tree()
    tree.feed((ROOT / "secure_frontend/web/index.html").read_text(encoding="utf-8"))
    for name in ("generation", "memory", "speed", "template", "calibration", "files"):
        attrs, _ = tree.nodes["model-card-tab-" + name]
        assert attrs["role"] == "tab"
        assert attrs["aria-controls"] == "model-card-panel-" + name
        assert attrs["aria-selected"] == ("true" if name == "generation" else "false")
        assert attrs["tabindex"] == ("0" if name == "generation" else "-1")
    for name, child in (("template", "model-template"), ("calibration", "model-calibration"),
                        ("files", "model-folder"), ("files", "model-pin"), ("files", "model-delete")):
        assert "model-card-panel-" + name in tree.nodes[child][1]
    for child in ("model-start", "model-stop", "model-progress", "model-start-error"):
        assert "model-selection" in tree.nodes[child][1]
        assert "model-detail" not in tree.nodes[child][1]
    assert "model-detail" in tree.nodes["model-defaults"][1]


def test_card_tab_navigation_preserves_values_and_reveals_invalid_fields():
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node is needed for the DOM event harness")
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    code = source[source.index("  function labelOf("):source.index("  // ---------------------------------------------------------------- template review")]
    harness = r'''
const assert = require('node:assert/strict');
let focused;
function el(tag, cls, text) {
  const n = {tag, className:cls || '', textContent:text || '', children:[], attrs:{}, events:{}, value:'',
    appendChild(c) { this.children.push(c); c.parent=this; c.parentNode=this; return c; },
    setAttribute(k,v) { this.attrs[k]=v; }, getAttribute(k) { return this.attrs[k]; },
    addEventListener(k,f) { (this.events[k] ||= []).push(f); }, focus() { focused=this; },
    emit(k, e={}) { (this.events[k] || []).forEach(f=>f(e)); if(this.parent) this.parent.emit(k,e); }};
  n.classList={add(c) {n.className+=' '+c;},remove(c) {n.className=n.className.split(' ').filter(x=>x!==c).join(' ');},
    toggle(c,on) {this.remove(c);if(on)this.add(c);}};
  Object.defineProperty(n,'options',{get(){return n.children;}});
  return n;
}
const ids={};
const box=ids['model-options']=el('div');
function all(n){return [n,...n.children.flatMap(all)];}
const $=id=>ids[id] || all(box).find(n=>n.id===id);
const clear=n=>{n.children=[];};
const document={createTextNode:t=>el('#text','',t)};
const num=String,fmt=String,valueText=String,DOT=' ',hostGpus=[];
const launcher={};
function showStartError(){}
['generation','memory','speed','template','calibration','files'].forEach(name=>{
  ids['model-card-tab-'+name]=el('button');
  if(['template','calibration','files'].includes(name)) ids['model-card-panel-'+name]=el('section');
});
ids['model-tradeoff-warning']=el('p');
const temperature={name:'temperature',kind:'decimal',min:0,max:2,default:.6,value:.375};
const context={name:'max_context',kind:'integer',choices:[1024,2048,4096],default:4096,value:4096};
const kv={name:'kv_resident',kind:'integer',choices:[0,1024,2048,4096],default:2048,value:2048,at_most:'max_context'};
const fast={name:'speed_fast_math',kind:'boolean',default:false,value:true,accuracy_tradeoff:true,accuracy_default:false};
const skip={name:'speed_tail_skip',kind:'decimal',min:0,max:1,default:0,value:0,accuracy_tradeoff:true,accuracy_default:0};
const card={name:'one',options:[temperature,context,kv,fast,skip,{name:'future_option',kind:'boolean',default:false,value:true}]};
buildOptions(card);
initCardTabs();
const named=name=>launcher.controls.find(c=>c.option.name===name);
assert.equal(named('temperature').tab,'generation');
assert.equal(named('future_option').tab,'generation','new options remain visible');
assert.equal(named('max_context').tab,'memory');
assert.equal(named('kv_resident').tab,'memory');
assert.equal(named('speed_fast_math').tab,'speed');
assert.equal(launcher.cardTab,'generation');
assert.equal($('model-card-panel-memory').hidden,true);
assert.equal($('model-tradeoff-warning').hidden,false,'active tradeoffs visible outside Speed tab');
$('model-card-tab-memory').emit('click');
assert.equal(launcher.cardTab,'memory');
assert.equal($('model-card-tab-memory').attrs['aria-selected'],'true');
assert.equal($('model-card-panel-generation').hidden,true);
named('max_context').element.value='1024'; named('max_context').element.emit('change');
assert.equal(named('kv_resident').read().value,1024,'dependent memory bound still applies');
let prevented=false;
$('model-card-tab-memory').emit('keydown',{key:'ArrowRight',preventDefault(){prevented=true;}});
assert.ok(prevented); assert.equal(launcher.cardTab,'speed');
assert.equal(focused,$('model-card-tab-speed'));
$('model-card-tab-speed').emit('keydown',{key:'End',preventDefault(){}});
assert.equal(launcher.cardTab,'files');
$('model-card-tab-files').emit('keydown',{key:'ArrowRight',preventDefault(){}});
assert.equal(launcher.cardTab,'generation');
$('model-card-tab-generation').emit('keydown',{key:'ArrowLeft',preventDefault(){}});
assert.equal(launcher.cardTab,'files');
$('model-card-tab-files').emit('keydown',{key:'Home',preventDefault(){}});
assert.equal(launcher.cardTab,'generation');
const values=collectOptions().values;
assert.equal(values.temperature,.375); assert.equal(values.kv_resident,1024);
assert.equal(values.speed_fast_math,true); assert.equal(values.future_option,true);
named('temperature').set(9); showCardTab('files');
assert.ok(collectOptions().error);
assert.equal(launcher.cardTab,'generation','validation reveals first hidden error');
assert.equal(focused,named('temperature').field);
named('temperature').set(.725); named('speed_tail_skip').set(2); showCardTab('files');
assert.ok(collectOptions().error); assert.equal(launcher.cardTab,'speed');
assert.equal(named('speed_tail_skip').field.parentNode.parentNode.open,true,'hidden disclosure opens for invalid tradeoff');
named('speed_tail_skip').set(0); showCardTab('memory'); showCardTab('generation');
assert.equal(named('temperature').read().value,.725,'navigation never rebuilds the form');
showCardTab('files'); buildOptions(card);
assert.equal(launcher.cardTab,'files','refresh retains selected tab');
buildOptions({name:'empty',options:[]});
assert.equal(launcher.cardTab,'generation','new card starts at generation');
assert.equal($('model-tradeoff-warning').hidden,true);
assert.ok($('model-card-panel-memory').children.length,'empty tabs explain unavailable settings');
'''
    result = subprocess.run([node, "-e", code + harness], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr


def test_selection_launch_controls_follow_selected_and_running_models():
    node = shutil.which("node")
    if node is None:
        pytest.skip("Node is needed for the DOM event harness")
    source = (ROOT / "secure_frontend/web/app.js").read_text(encoding="utf-8")
    start = source.index("  function renderDetail(")
    code = source[start:source.index("\n  // GUI-first start-up", start)]
    harness = r'''
const assert=require('node:assert/strict');
const nodes={};
const $=id=>nodes[id] ||= {appendChild(){}};
const launcher={data:{state:'stopped',running:''},controls:[{}],acting:false};
function clear(){} function resetTemplateReview(){} function stateBadge(){} function renderCalibration(){}
function profileText(){return 'Test engine';} function progressText(){return 'Ready';}
function launcherStarting(d){return d.state==='starting';}
const first={name:'First',state:'stopped'},second={name:'Second',state:'stopped'};
renderDetail(first,false);
assert.equal($('model-launch-name').textContent,'First');
assert.equal($('model-start').textContent,'Start');
assert.equal($('model-start').disabled,false); assert.equal($('model-stop').disabled,true);
launcher.data={state:'running',running:'First'}; first.state='running'; renderDetail(first,false);
assert.equal($('model-start').textContent,'Restart'); assert.equal($('model-stop').disabled,false);
renderDetail(second,false);
assert.equal($('model-launch-name').textContent,'Second'); assert.equal($('model-start').textContent,'Start');
assert.equal($('model-stop').title,'Stop First','Stop identifies the running model even when another is selected');
launcher.acting=true; renderDetail(second,false);
assert.equal($('model-start').disabled,true); assert.equal($('model-stop').disabled,true);
launcher.acting=false; launcher.data.state='starting'; renderDetail(second,false);
assert.equal($('model-start').disabled,true);
renderDetail(null,false); assert.equal($('model-launch-controls').hidden,true);
'''
    result = subprocess.run([node, "-e", code + harness], capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stdout + result.stderr
