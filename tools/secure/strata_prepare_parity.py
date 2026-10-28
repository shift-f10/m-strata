#!/usr/bin/env python3
"""strata-prepare against the Python tools it ports, byte for byte (spec 2026-10-27-strata-prepare-design.md).

    python -I tools/secure/strata_prepare_parity.py --exe <strata-prepare> --gguf-py <llama.cpp/gguf-py> --work <scratch dir>
        [--tools <upstream Strata tools dir> [--real <shard 1> [--real-compat]]]
        [--maya-tools <Project Maya v1.1.30 tools dir> [--maya-real <shard 1>]]
        [--raon-tools <those tools with our raon1 additions> [--raon-real <shard 1>]]

A dev-only check (Python is never needed to run strata-prepare). For each case it writes a synthetic GGUF with llama.cpp's
gguf-py, runs the Python tool (this Python, numpy, regex, STRATA_GGUF_PY) and strata-prepare on it into two folders, and
compares the exit codes and every file:
  --tools        upstream's iq_pack.py against `strata-prepare pack`, every file but tokenizer/ (upstream's tokenizer
                 step is not ported: the server reads a Qwen3.8 tokenizer from the GGUF; a stub tokenizer/ makes
                 iq_pack.py skip it). --real adds a real model.
  --maya-tools   Maya's fork of iq_pack.py against `strata-prepare pack --maya`, tokenizer/ included (Maya's
                 strata_tokenizer.py, which its iq_pack.py runs); Maya's strata_tokenizer.py against `strata-prepare
                 tokenizer`; and Maya's setup step (iq_pack.py --compat-bf16, then maya.py's stamp_pack, copied below)
                 against `strata-prepare download --maya` (its prepared.json aside). --maya-real adds a real GLM model
                 that way (its files are only read; both packs go under --work).
  --raon-tools   raon1 the same way, against our additions to those two Python tools (engines/maya-ref/tools' diff on
                 Maya v1.1.30's): synthetic Q8_0 and IQ4_XS-layout models, the setup step and the tokenizer; --raon-real
                 adds a real raon-1-mini GGUF.
Prints one line per case and exits non-zero on any difference.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

import numpy as np

A = None          # parsed arguments
Q = None          # gguf.GGMLQuantizationType
GGUFWriter = None
GGML_QUANT_SIZES = None

# The f16 scale fields of each block type (offsets within a block), set to sane values so dequantized weights stay
# finite; the rest of every block is random.
SCALES = {
    "Q8_0": (34, [0]), "Q4_0": (18, [0]), "Q4_1": (20, [0, 2]), "Q5_0": (22, [0]), "Q5_1": (24, [0, 2]),
    "Q2_K": (84, [80, 82]), "Q3_K": (110, [108]), "Q4_K": (144, [0, 2]), "Q5_K": (176, [0, 2]), "Q6_K": (210, [208]),
    "IQ4_NL": (18, [0]), "IQ4_XS": (136, [0]), "IQ2_XXS": (66, [0]), "IQ2_XS": (74, [0]), "IQ3_XXS": (98, [0]),
    "IQ3_S": (110, [0]), "IQ2_S": (82, [0]), "IQ1_S": (50, [0]), "MXFP4": (17, []),
}
BLOCK = {"Q8_0": 32, "Q4_0": 32, "Q4_1": 32, "Q5_0": 32, "Q5_1": 32, "IQ4_NL": 32, "MXFP4": 32}   # else 256


def quant(rng, qtype: str, elements: int) -> np.ndarray:
    size, scales = SCALES[qtype]
    block = BLOCK.get(qtype, 256)
    assert elements % block == 0, (qtype, elements)
    n = elements // block
    raw = rng.integers(0, 256, size=(n, size), dtype=np.uint8)
    for off in scales:
        raw[:, off:off + 2] = np.frombuffer(rng.uniform(0.002, 0.05, n).astype(np.float16).tobytes(), dtype=np.uint8).reshape(n, 2)
    return raw.reshape(-1)


class Model:
    """Tensors for one GGUF: (name, GGUF shape, type name, raw bytes)."""

    def __init__(self, seed: int):
        self.rng = np.random.default_rng(seed)
        self.tensors = []

    def floats(self, name, shape, kind="F32", values=None):
        count = int(np.prod(shape))
        v = self.rng.standard_normal(count).astype(np.float32) * 0.1 if values is None else np.asarray(values, np.float32)
        if kind == "F32":
            raw = v.astype("<f4").view(np.uint8)
        elif kind == "F16":
            raw = v.astype("<f2").view(np.uint8)
        else:   # BF16 from the top half of float32
            raw = (v.view(np.uint32) >> 16).astype("<u2").view(np.uint8)
        self.tensors.append((name, list(shape), kind, raw))

    def bf16_exact(self, name, shape):
        count = int(np.prod(shape))
        v = (self.rng.standard_normal(count).astype(np.float32).view(np.uint32) & np.uint32(0xFFFF0000)).view(np.float32)
        self.floats(name, shape, "F32", v)

    def q(self, name, shape, qtype):
        self.tensors.append((name, list(shape), qtype, quant(self.rng, qtype, int(np.prod(shape)))))

    def experts(self, layer, n_embd, n_ff, n_expert, gu="IQ3_S", down="Q4_K"):
        b = f"blk.{layer}."
        self.q(b + "ffn_gate_exps.weight", [n_embd, n_ff, n_expert], gu)
        self.q(b + "ffn_up_exps.weight", [n_embd, n_ff, n_expert], gu)
        self.q(b + "ffn_down_exps.weight", [n_ff, n_embd, n_expert], down)


def write(path: pathlib.Path, tensors, kv=None, arch="qwen4exp"):
    w = GGUFWriter(str(path), arch)
    for key, (kind, value) in (kv or {}).items():
        getattr(w, "add_" + kind)(key, value)
    for name, shape, kind, raw in tensors:
        # gguf-py takes uint8 data in its byte shape (the GGUF shape reversed, the row in encoded bytes) and converts it
        block, size = GGML_QUANT_SIZES[Q[kind]]
        byte_shape = list(reversed(shape))
        byte_shape[-1] = byte_shape[-1] // block * size
        w.add_tensor(name, np.ascontiguousarray(raw).reshape(byte_shape), raw_dtype=Q[kind])
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()


def base_model(seed=1, n_layers=2, n_embd=256, n_ff=256, n_expert=4) -> Model:
    m = Model(seed)
    m.q("token_embd.weight", [n_embd, 64], "Q8_0")
    m.floats("output_norm.weight", [n_embd])
    m.q("output.weight", [n_embd, 64], "Q6_K")
    m.q("per_layer_token_embd.weight", [n_embd, 64], "Q8_0")
    for l in range(n_layers):
        b = f"blk.{l}."
        m.floats(b + "ffn_gate_inp.weight", [n_embd, n_expert], "BF16")
        m.floats(b + "attn_q_norm.weight", [n_embd], "F16")        # FORM F32: widened
        m.floats(b + "attn_k_norm.weight", [n_embd], "BF16")       # FORM F32: widened
        m.bf16_exact(b + "ssm_alpha.weight", [n_embd, 32])          # FORM BF16 from exact F32
        m.floats(b + "ssm_a", [32])                                 # FORM F32 as stored
        m.floats(b + "ple_conv1d.weight", [4, n_embd])              # FORM F16 from F32: index kind 3
        m.q(b + "attn_q.weight", [n_embd, n_embd], "IQ4_XS")        # no FORM: served natively
        m.experts(l, n_embd, n_ff, n_expert)
    return m


def compat_model() -> Model:
    m = base_model(seed=2)
    n = 256
    m.q("blk.0.hc_attn_down.weight", [n, 4], "Q8_0")
    m.q("blk.0.hc_attn_up.weight", [n, 4], "Q4_K")
    m.q("blk.0.hc_ffn_down.weight", [n, 4], "Q6_K")
    m.q("blk.0.hc_ffn_up.weight", [n, 4], "Q5_K")
    m.q("blk.1.hc_attn_down.weight", [n, 4], "IQ4_XS")
    m.q("blk.1.hc_attn_up.weight", [n, 4], "Q3_K")
    m.q("blk.1.hc_ffn_down.weight", [n, 4], "Q2_K")
    m.q("blk.1.hc_ffn_up.weight", [n, 4], "IQ3_XXS")
    m.q("blk.0.indexer.k_proj.weight", [n, 4], "IQ2_XS")
    m.q("blk.0.indexer.q_proj.weight", [n, 4], "Q5_0")
    m.q("blk.1.indexer.k_proj.weight", [n, 4], "Q4_1")
    m.q("blk.1.indexer.q_proj.weight", [n, 4], "IQ4_NL")
    m.q("blk.0.ple_value.weight", [n, 4], "Q8_0")
    m.floats("blk.0.ple_key.weight", [n, 4], "F16")                # F16 -> BF16 rounding
    m.floats("blk.0.ssm_beta.weight", [n, 32], "F32")              # F32 not BF16 -> rounding
    m.q("blk.1.ple_conv1d.weight", [32, 8], "Q8_0")                 # -> F16 (compat); rows of whole blocks
    m.q("blk.0.ssm_norm.weight", [n], "Q8_0")                      # -> F32 widening (compat)
    m.q("blk.1.ple_key.weight", [n, 4], "Q8_0")                    # the native PLE key: served
    m.tensors = [t for t in m.tensors if t[0] != "blk.1.ple_conv1d.weight" or t[2] == "Q8_0"]
    m.tensors = [t for t in m.tensors if not (t[0] == "blk.1.ple_conv1d.weight" and t[2] == "F32")]
    return m


def tokenizer_kv(pre="glm4", types=True, template=True) -> dict:
    """A byte-level BPE tokenizer: a non-ASCII token, one needing JSON escapes, a control token, one of invalid UTF-8
    (gguf-py writes bytes as they are), a merge listed twice, the specials out of key order, a template with a CRLF."""
    kv = {"tokenizer.ggml.model": ("string", "gpt2")}
    if pre:
        kv["tokenizer.ggml.pre"] = ("string", pre)
    kv["tokenizer.ggml.tokens"] = ("array", ["a", "b", "ab", "é", "q\"\\", "<|end|>", "\x01", b"bad\xff", "Ġthe", "Ċ"])
    kv["tokenizer.ggml.merges"] = ("array", ["a b", "ab a", "a b"])
    if types:
        kv["tokenizer.ggml.token_type"] = ("array", [1, 1, 1, 1, 1, 3, 1, 1, 1, 1])
    kv["tokenizer.ggml.eos_token_id"] = ("uint32", 5)
    kv["tokenizer.ggml.bos_token_id"] = ("uint32", 0)
    if template:
        kv["tokenizer.chat_template"] = ("string", "[gMASK]<sop>{% for m in messages %}{{ m.content }}{% endfor %}\r\nend\n")
    return kv


def glm_model(seed=11, arch="glm5-next", blocks=4, nextn=1, lead=1, gu="IQ2_XXS", down="Q4_K", odd=False):
    """A small GLM-5.3-like model: `blocks` counting `nextn` NextN blocks and `lead` leading dense ones; n_embd 256, 4
    experts. Quantized tensors of the native set, the BF16 list in F16 / BF16 / F32 / Q8_0 / Q6_K (3-D absorbed
    projections, a 4-D conv weight with a trailing 1), a NextN block with a tensor outside the native set (dropped)."""
    m = Model(seed)
    n = 256
    m.q("token_embd.weight", [n, 64], "Q4_K")
    m.floats("output_norm.weight", [n])
    m.q("output.weight", [n, 64], "Q6_K")
    for l in range(blocks):
        b = f"blk.{l}."
        m.floats(b + "attn_norm.weight", [n])
        m.floats(b + "attn_q_a_norm.weight", [64], "F16")
        m.floats(b + "attn_kv_a_norm.weight", [64], "BF16")
        m.q(b + "attn_q_a.weight", [n, 64], "Q5_K")
        m.q(b + "attn_q_b.weight", [n, 64], "IQ4_XS")
        m.q(b + "attn_kv_a_mqa.weight", [n, 64], "Q8_0")
        m.floats(b + "attn_k_b.weight", [16, 16, 4], "F16")
        m.q(b + "attn_v_b.weight", [32, 8, 4], "Q8_0")
        m.q(b + "attn_output.weight", [n, n], "Q4_K")
        m.floats(b + "ssm_conv1d_q.weight", [4, 1, n, 1])
        m.q(b + "hc_attn_fn.weight", [n, 4], "Q8_0")
        m.floats(b + "hc_attn_base.weight", [4])
        m.q(b + "indexer.attn_k.weight", [n, 64], "Q6_K")
        m.floats(b + "indexer.k_norm.bias", [64])
        if odd and l == 1:
            m.q(b + "attn_sink.weight", [n], "Q8_0")       # quantized, on neither list: refused
        if l < lead:
            m.q(b + "ffn_gate.weight", [n, n], "Q4_K")
            m.q(b + "ffn_up.weight", [n, n], "Q4_K")
            m.q(b + "ffn_down.weight", [n, n], "Q6_K")
            continue
        m.floats(b + "ffn_gate_inp.weight", [n, 4])
        m.floats(b + "exp_probs_b.bias", [4])
        m.q(b + "ffn_gate_shexp.weight", [n, n], "Q8_0")
        m.q(b + "ffn_up_shexp.weight", [n, n], "Q8_0")
        m.q(b + "ffn_down_shexp.weight", [n, n], "Q8_0")
        if l >= blocks - nextn:
            m.q(b + "eh_proj.weight", [2 * n, n], "Q8_0")
            m.floats(b + "enorm.weight", [n])
        m.experts(l, n, n, 4, gu, down)
    kv = tokenizer_kv()
    kv.update({f"{arch}.block_count": ("uint32", blocks), f"{arch}.nextn_predict_layers": ("uint32", nextn),
               f"{arch}.leading_dense_block_count": ("uint32", lead)})
    return m, kv


def raon1_model(seed=41, blocks=3, lead=1, gu="Q8_0", down="Q8_0", quantized_kb=False):
    """A small raon-1-mini-like model: `blocks` (no NextN) with `lead` leading dense blocks; n_embd 256, 4 experts. Its
    small projections F16 / F32 as raon1 ships them (3-D absorbed ones among them), the RAON1_NATIVE_OK tensors Q8_0;
    `quantized_kb`: attn_k_b / attn_v_b quantized as the IQ4_XS build has them (dequantized to F32 into dense.bin)."""
    m = Model(seed)
    n = 256
    m.q("token_embd.weight", [n, 64], "Q8_0")
    m.floats("output_norm.weight", [n])
    m.q("output.weight", [n, 64], "Q8_0")
    for l in range(blocks):
        b = f"blk.{l}."
        m.floats(b + "attn_norm.weight", [n])
        m.q(b + "attn_q_a.weight", [n, 64], "Q8_0")
        m.floats(b + "attn_q_a_norm.weight", [64])
        m.q(b + "attn_q_b.weight", [64, 128], "Q8_0")
        m.floats(b + "attn_kv_a_mqa.weight", [n, 96], "F16")
        m.floats(b + "attn_kv_a_norm.weight", [64])
        if quantized_kb:
            m.q(b + "attn_k_b.weight", [32, 2, 4], "Q8_0")
            m.q(b + "attn_v_b.weight", [64, 1, 4], "Q8_0")
        else:
            m.floats(b + "attn_k_b.weight", [16, 16, 4])
            m.floats(b + "attn_v_b.weight", [32, 8, 4])
        m.q(b + "attn_gate.weight", [n, 128], "Q8_0")
        m.q(b + "attn_output.weight", [128, n], "Q8_0")
        m.floats(b + "ssm_conv1d_q.weight", [4, n])
        m.floats(b + "ssm_rda.weight", [n, 8], "F16")
        m.q(b + "ssm_g.weight", [n, n], "Q8_0")
        m.floats(b + "hc_attn_fn.weight", [n, 17], "F16")
        m.floats(b + "hc_attn_base.weight", [17])
        if l < lead:
            m.q(b + "ffn_gate.weight", [n, n], "Q8_0")
            m.q(b + "ffn_up.weight", [n, n], "Q8_0")
            m.q(b + "ffn_down.weight", [n, n], "Q8_0")
            continue
        m.floats(b + "ffn_gate_inp.weight", [n, 4])
        m.floats(b + "exp_probs_b.bias", [4])
        m.q(b + "ffn_gate_shexp.weight", [n, n], "Q8_0")
        m.q(b + "ffn_up_shexp.weight", [n, n], "Q8_0")
        m.q(b + "ffn_down_shexp.weight", [n, n], "Q8_0")
        m.experts(l, n, n, 4, gu, down)
    kv = tokenizer_kv("raon")
    kv.update({"raon1.block_count": ("uint32", blocks), "raon1.leading_dense_block_count": ("uint32", lead)})
    return m, kv


# maya.py (v1.1.30) stamp_pack, as it is there; PACK_SOURCES = "sources.json"
def stamp_pack(pack: pathlib.Path, shards: list) -> None:
    """The model files' sizes and times into the pack (PACK_SOURCES), for pack_stale."""
    files = [{"name": s.name, "size": s.stat().st_size, "mtime_ns": s.stat().st_mtime_ns} for s in shards]
    try:
        (pack / "sources.json").write_text(json.dumps({"files": files}, indent=1), encoding="utf-8")
    except OSError:
        pass


def shards_of(first: pathlib.Path) -> list:
    m = re.search(r"-(\d{5})-of-(\d{5})\.gguf$", first.name)
    if not m:
        return [first]
    return [first.with_name(first.name[:m.start()] + "-%05d-of-%05d.gguf" % (i, int(m.group(2)))) for i in range(1, int(m.group(2)) + 1)]


def run(cmd, env=None) -> int:
    done = subprocess.run(cmd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    return done.returncode


def report(name: str, py: pathlib.Path, cc: pathlib.Path, rp: int, rc: int, skip=lambda rel: False) -> bool:
    files_py = {p.relative_to(py).as_posix() for p in py.rglob("*") if p.is_file() and not skip(p.relative_to(py))}
    files_cc = {p.relative_to(cc).as_posix() for p in cc.rglob("*") if p.is_file() and p.name != "prepared.json"}
    problems = []
    if rp != rc:
        problems.append(f"exit codes: Python {rp}, strata-prepare {rc}")
    if files_py != files_cc:
        problems.append(f"files: only Python {sorted(files_py - files_cc)}, only strata-prepare {sorted(files_cc - files_py)}")
    for f in sorted(files_py & files_cc):
        a, b = (py / f).read_bytes(), (cc / f).read_bytes()
        if a != b:
            at = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            problems.append(f"{f}: differs at byte {at} (sizes {len(a)} / {len(b)})")
    print(("ok   " if not problems else "DIFF ") + f"{name}: exit {rp}, {len(files_py)} files" +
          ("" if not problems else "\n       " + "\n       ".join(problems)), flush=True)
    return not problems


def fresh(name: str):
    work = pathlib.Path(A.work) / name
    py, cc = work / "python", work / "native"
    for d in (py, cc):
        shutil.rmtree(d, ignore_errors=True)
    return py, cc


def compare_maya(name: str, gguf: pathlib.Path, flags: list[str], setup=False, tools=None) -> bool:
    """Maya's iq_pack.py against `strata-prepare pack --maya`; `setup`: Maya's setup step (--compat-bf16 and
    stamp_pack) against `strata-prepare download --maya`. `tools`: another tools folder (--raon-tools)."""
    py, cc = fresh(name)
    env = dict(os.environ, STRATA_GGUF_PY=A.gguf_py)
    py_flags = ["--compat-bf16"] if setup else flags
    rp = run([sys.executable, str(pathlib.Path(tools or A.maya_tools) / "iq_pack.py"), "--gguf", str(gguf), "--out", str(py), *py_flags], env)
    if setup and rp == 0:
        stamp_pack(py, shards_of(gguf))
    if setup:
        rc = run([A.exe, "download", "--maya", "--gguf", str(gguf), "--out", str(cc)])
    else:
        rc = run([A.exe, "pack", "--maya", "--gguf", str(gguf), "--out", str(cc), *flags])
    return report(name, py, cc, rp, rc)


def ints(m: Model, name: str, values, kind="I64"):
    m.tensors.append((name, [len(values)], kind, np.asarray(values, "<i8" if kind == "I64" else "<i4").view(np.uint8)))


def ds4_model(seed=21, variant="engram") -> tuple:
    """A small DeepSeek-V4.1 GGUF (two layers, four experts): BF16 / F32 / Q8_0 dense tensors, MXFP4 routed experts, a
    two-group output projection, f32 metadata Python prints in full, and the Engram tables of layer 1 three ways:
    `engram` - the engram.* keys; `newer` - newer converters' key names, the hash parameters as I64 / I32 tensors;
    `ple` - llama.cpp's ple.* keys with one per_layer_token_embd tensor (split per layer) and a 3-D output projection."""
    m = Model(seed)
    n = 256
    m.floats("token_embd.weight", [n, 16], "BF16")
    m.floats("output_norm.weight", [n])
    m.q("output.weight", [n, 16], "Q8_0")
    for l in range(2):
        b = f"blk.{l}."
        m.floats(b + "attn_norm.weight", [n])
        m.floats(b + "attn_sinks.weight", [4])
        m.q(b + "attn_q_a.weight", [n, 64], "Q8_0")
        m.floats(b + "attn_q_a_norm.weight", [64])
        if variant == "ple":
            m.floats(b + "attn_output_a.weight", [64, 32, 2], "BF16")
        else:
            m.floats(b + "attn_output_a.weight", [64, 64], "BF16")
        m.q(b + "attn_output_b.weight", [64, n], "Q8_0")
        m.floats(b + "hc_attn_fn.weight", [16, 4])
        m.floats(b + "ffn_gate_inp.weight", [n, 4])
        m.floats(b + "exp_probs_b.bias", [4])
        m.experts(l, n, n, 4, "MXFP4", "MXFP4")
    if variant != "ple":
        m.floats("blk.1.engram_embd.weight", [8, 20], "F16")
    m.q("blk.1.engram_kv.weight", [32, 16], "Q8_0")
    m.floats("blk.1.engram_gate.weight", [16])
    a = "deepseek41"
    kv = tokenizer_kv("deepseek3")
    kv.update({f"{a}.block_count": ("uint32", 2), f"{a}.embedding_length": ("uint32", n), f"{a}.attention.head_count": ("uint32", 4),
               f"{a}.attention.key_length": ("uint32", 64), f"{a}.rope.dimension_count": ("uint32", 16),
               f"{a}.attention.q_lora_rank": ("uint32", 64), f"{a}.attention.output_group_count": ("uint32", 2),
               f"{a}.expert_count": ("uint32", 4), f"{a}.expert_used_count": ("uint32", 2),
               f"{a}.expert_feed_forward_length": ("uint32", n), f"{a}.hyper_connection.epsilon": ("float32", 1e-6),
               f"{a}.attention.layer_norm_rms_epsilon": ("float32", 1e-6), f"{a}.swiglu_clamp_exp": ("array", [7.0, 7.0]),
               f"{a}.rope.freq_base": ("float32", 10000.0), f"{a}.attention.compress_ratios": ("array", [0, 4]),
               f"{a}.rope.scaling.factor": ("float32", 16.0), f"{a}.context_length": ("uint32", 1048576)})
    tm = [i % 9 for i in range(12)]
    if variant == "engram":
        kv.update({f"{a}.engram.layers": ("array", [1]), f"{a}.engram.max_ngram_size": ("uint32", 3),
                   f"{a}.engram.head_count": ("uint32", 2), f"{a}.engram.head_dim": ("uint32", 8),
                   f"{a}.engram.multipliers": ("array", [3, 5, 7]), f"{a}.engram.primes": ("array", [5, 4, 5, 6]),
                   f"{a}.engram.offsets": ("array", [0, 5, 9, 14]), f"{a}.engram.token_map": ("array", tm),
                   f"{a}.engram.pad_token_id": ("uint32", 2)})
    else:
        kv.update({f"{a}.engram_max_ngram_size": ("uint32", 3), f"{a}.engram_n_heads": ("uint32", 2),
                   f"{a}.engram_head_dim": ("uint32", 8), f"{a}.engram_pad_token_id": ("uint32", 2),
                   f"{a}.kv_source_layer_ids": ("array", [0, 1])})
        ints(m, "engram_multipliers.weight", [3, 5, 7])
        ints(m, "engram_token_map.weight", tm, "I32")
        if variant == "newer":
            kv[f"{a}.engram.layer_ids"] = ("array", [1])
            ints(m, "engram_primes.weight", [5, 4, 5, 6])
            ints(m, "engram_offsets.weight", [0, 5, 9, 14])
        else:   # ple: the per-layer offsets counted across the layers' tables, one Engram tensor
            kv.update({f"{a}.ple.layers": ("array", [1]), f"{a}.ple.ngram_size": ("uint32", 3),
                       f"{a}.ple.heads_per_ngram": ("uint32", 2), f"{a}.ple.head_offsets": ("array", [0, 5, 9, 14]),
                       f"{a}.ple.head_vocab_sizes": ("array", [5, 4, 5, 6]), f"{a}.ple.layer_multipliers": ("array", [3, 5, 7]),
                       f"{a}.embedding_length_per_layer_input": ("uint32", 8)})
            m.floats("per_layer_token_embd.weight", [8, 20], "F16")
    return m, kv


def mm3_model(seed=31) -> tuple:
    """A small MiniMax-M3 GGUF set: three layers (the first dense), IQ1_S / IQ2_XXS experts, Q4_K..Q8_0 dense matrices;
    layer 0's indexers in the -maya-indexers side file (the main set has them from the first MoE layer on)."""
    m, side = Model(seed), Model(seed + 1)
    n = 256
    m.floats("token_embd.weight", [n, 16], "BF16")
    m.floats("output_norm.weight", [n])
    m.q("output.weight", [n, 16], "Q6_K")
    for l in range(3):
        b = f"blk.{l}."
        m.floats(b + "attn_norm.weight", [n])
        m.floats(b + "ffn_norm.weight", [n])
        m.q(b + "attn_q.weight", [n, 256], "Q4_K")
        m.q(b + "attn_k.weight", [n, 64], "Q5_K")
        m.q(b + "attn_v.weight", [n, 64], "Q8_0")
        m.q(b + "attn_output.weight", [256, n], "Q6_K")
        m.floats(b + "attn_q_norm.weight", [64])
        m.floats(b + "attn_k_norm.weight", [64])
        idx = side if l == 0 else m
        idx.q(b + "indexer.q_proj.weight", [n, 64], "Q8_0")
        idx.q(b + "indexer.k_proj.weight", [n, 64], "Q8_0")
        idx.floats(b + "indexer.q_norm.weight", [64])
        idx.floats(b + "indexer.k_norm.weight", [64])
        if l == 0:
            m.q(b + "ffn_gate.weight", [n, 512], "Q4_K")
            m.q(b + "ffn_up.weight", [n, 512], "Q4_K")
            m.q(b + "ffn_down.weight", [512, n], "Q6_K")
            continue
        m.floats(b + "ffn_gate_inp.weight", [n, 4])
        m.floats(b + "exp_probs_b.bias", [4])
        m.q(b + "ffn_gate_shexp.weight", [n, 256], "Q8_0")
        m.q(b + "ffn_up_shexp.weight", [n, 256], "Q8_0")
        m.q(b + "ffn_down_shexp.weight", [256, n], "Q8_0")
        m.experts(l, n, n, 4, "IQ1_S", "IQ2_XXS")
    a = "minimax-m3"
    kv = tokenizer_kv("minimax")
    kv.update({"general.name": ("string", "MiniMax M3.1 Test"), "maya.recipe": ("string", '{"tier": "XS", "layers": [1, 2]}'),
               f"{a}.block_count": ("uint32", 3), f"{a}.leading_dense_block_count": ("uint32", 1),
               f"{a}.embedding_length": ("uint32", n), f"{a}.attention.head_count": ("uint32", 4),
               f"{a}.attention.head_count_kv": ("uint32", 1), f"{a}.attention.key_length": ("uint32", 64),
               f"{a}.rope.dimension_count": ("uint32", 32), f"{a}.expert_count": ("uint32", 4),
               f"{a}.expert_used_count": ("uint32", 2), f"{a}.expert_feed_forward_length": ("uint32", n),
               f"{a}.feed_forward_length": ("uint32", 512), f"{a}.expert_shared_count": ("uint32", 1),
               f"{a}.rope.freq_base": ("float32", 5000000.0), f"{a}.attention.layer_norm_rms_epsilon": ("float32", 1e-6),
               f"{a}.context_length": ("uint32", 196608), f"{a}.attention.indexer.head_count": ("uint32", 2),
               f"{a}.attention.indexer.key_length": ("uint32", 64), f"{a}.attention.indexer.top_k": ("uint32", 16),
               f"{a}.attention.indexer.block_size": ("uint32", 64), f"{a}.expert_weights_scale": ("float32", 2.5)})
    return m, side, kv


def compare_moe(name: str, family: str, model: pathlib.Path) -> bool:
    """Maya's tools/<family>/pack_gguf.py against `strata-prepare pack --maya-<family>` (with Maya's chat template)."""
    py, cc = fresh(name)
    tools = pathlib.Path(A.maya_tools)
    rp = run([sys.executable, str(tools / family / "pack_gguf.py"), "--model", str(model), "--out", str(py)])
    rc = run([A.exe, "pack", "--maya-" + family, "--model", str(model), "--out", str(cc),
              "--template", str(tools / family / "chat_template.jinja")])
    return report(name, py, cc, rp, rc)


def compare_tokenizer(name: str, gguf: pathlib.Path, tools=None) -> bool:
    py, cc = fresh(name)
    rp = run([sys.executable, str(pathlib.Path(tools or A.maya_tools) / "strata_tokenizer.py"), "--gguf", str(gguf), "--out", str(py)])
    rc = run([A.exe, "tokenizer", "--gguf", str(gguf), "--out", str(cc)])
    return report(name, py, cc, rp, rc)


def compare(name: str, gguf: pathlib.Path, flags: list[str]) -> bool:
    py, cc = fresh(name)
    (py / "tokenizer").mkdir(parents=True)
    (py / "tokenizer" / "vocab.json").write_text("{}", encoding="utf-8")            # iq_pack.py skips its tokenizer step
    (py / "tokenizer" / "chat_template.jinja").write_text("", encoding="utf-8")
    env = dict(os.environ, STRATA_GGUF_PY=A.gguf_py)
    rp = run([sys.executable, str(pathlib.Path(A.tools) / "iq_pack.py"), "--gguf", str(gguf), "--out", str(py), *flags], env)
    rc = run([A.exe, "pack", "--gguf", str(gguf), "--out", str(cc), *flags])
    return report(name, py, cc, rp, rc, skip=lambda rel: rel.parts[0] == "tokenizer")


def upstream_cases(work: pathlib.Path) -> bool:
    ok = True
    plain = work / "plain.gguf"
    write(plain, base_model().tensors)
    ok &= compare("plain", plain, [])
    ok &= compare("plain-experts-bin", plain, ["--experts-bin"])

    comp = work / "compat.gguf"
    write(comp, compat_model().tensors)
    ok &= compare("compat-refused", comp, [])
    ok &= compare("compat-bf16", comp, ["--compat-bf16"])

    # a split model: layer 1's down tensor and the PLE table in shard 2 (v4, per-role shard column)
    m = base_model(seed=3)
    second_names = {"blk.1.ffn_down_exps.weight", "per_layer_token_embd.weight"}
    first = [t for t in m.tensors if t[0] not in second_names]
    second = [t for t in m.tensors if t[0] in second_names]
    split_kv = lambda i: {"split.count": ("uint16", 2), "split.no": ("uint16", i), "split.tensors.count": ("int32", len(m.tensors))}
    write(work / "split-00001-of-00002.gguf", first, split_kv(0))
    write(work / "split-00002-of-00002.gguf", second, split_kv(1))
    ok &= compare("split-v4", work / "split-00001-of-00002.gguf", [])

    # Hadamard-folded experts (prism.hadamard, explicit signs): hadamard.txt and native_experts.txt v5
    m = base_model(seed=4)
    signs = [1 if x else -1 for x in np.random.default_rng(9).integers(0, 2, 256)]
    had = {"prism.hadamard.version": ("uint32", 1), "prism.hadamard.block_size": ("uint32", 64),
           "prism.hadamard.transform": ("string", "normalized-sylvester-walsh-hadamard"),
           "prism.hadamard.axis": ("string", "input-last-dimension"), "prism.hadamard.sign_mode": ("string", "explicit"),
           "prism.hadamard.weight_names": ("array", [f"blk.{l}.ffn_{r}_exps.weight" for l in range(2) for r in ("gate", "up", "down")]),
           "prism.hadamard.sign_widths": ("array", [256]), "prism.hadamard.sign_values": ("array", signs)}
    write(work / "hadamard.gguf", m.tensors, had)
    ok &= compare("hadamard-v5", work / "hadamard.gguf", [])

    for i, real in enumerate(A.real):
        ok &= compare(f"real-{i}", pathlib.Path(real), ["--compat-bf16"] if A.real_compat else [])
    return ok


def maya_cases(work: pathlib.Path) -> bool:
    ok = True
    m, kv = glm_model()
    write(work / "glm.gguf", m.tensors, kv, "glm5-next")
    ok &= compare_maya("glm-compat-bf16", work / "glm.gguf", ["--compat-bf16"])
    ok &= compare_maya("glm-refused", work / "glm.gguf", [])
    ok &= compare_maya("glm-experts-bin", work / "glm.gguf", ["--compat-bf16", "--experts-bin"])
    ok &= compare_maya("glm-setup", work / "glm.gguf", [], setup=True)
    ok &= compare_tokenizer("tokenizer-glm4", work / "glm.gguf")

    # unsloth's spelling, IQ3_XXS / IQ4_NL experts, two leading dense blocks
    m, kv = glm_model(seed=12, arch="glm5next", blocks=5, lead=2, gu="IQ3_XXS", down="IQ4_NL")
    write(work / "glm5next.gguf", m.tensors, kv, "glm5next")
    ok &= compare_maya("glm5next", work / "glm5next.gguf", ["--compat-bf16"])

    # a quantized tensor on neither list: both refuse, after the same partial dense.bin
    m, kv = glm_model(seed=13, odd=True)
    write(work / "glm-odd.gguf", m.tensors, kv, "glm5-next")
    ok &= compare_maya("glm-odd-refused", work / "glm-odd.gguf", ["--compat-bf16"])

    # three shards, a split inside layer 2 (its down in shard 3) and layer 3 whole in shard 2: v4, and a v3 row naming
    # its shard. No split keys: Maya's fork does not read them
    m, kv = glm_model(seed=14, blocks=5)
    third = {"blk.2.ffn_down_exps.weight"}
    second = {t[0] for t in m.tensors if t[0].startswith("blk.3.")}
    write(work / "glms-00001-of-00003.gguf", [t for t in m.tensors if t[0] not in second | third], kv, "glm5-next")
    write(work / "glms-00002-of-00003.gguf", [t for t in m.tensors if t[0] in second], {}, "glm5-next")
    write(work / "glms-00003-of-00003.gguf", [t for t in m.tensors if t[0] in third], {}, "glm5-next")
    ok &= compare_maya("glm-split", work / "glms-00001-of-00003.gguf", ["--compat-bf16"])
    ok &= compare_maya("glm-split-setup", work / "glms-00001-of-00003.gguf", [], setup=True)

    # Maya's qwen4exp route (no FORM table: an F32 router of BF16 values taken exactly, floats as stored); its other
    # BF16 projection (ssm_alpha, F32) left out, which this route would refuse without --compat-bf16
    q = base_model(seed=15)
    q.tensors = [t for t in q.tensors if not t[0].endswith(("ffn_gate_inp.weight", "ssm_alpha.weight"))]
    q.bf16_exact("blk.0.ffn_gate_inp.weight", [256, 4])
    q.bf16_exact("blk.1.ffn_gate_inp.weight", [256, 4])
    write(work / "maya-qwen.gguf", q.tensors, tokenizer_kv("qwen35"))
    ok &= compare_maya("maya-qwen4exp", work / "maya-qwen.gguf", [])

    # the tokenizer alone: no pre (qwen35), no token types, no template
    write(work / "tok-plain.gguf", [("w", [32], "F32", np.ones(32, np.float32).view(np.uint8))], tokenizer_kv("", False, False))
    ok &= compare_tokenizer("tokenizer-qwen35", work / "tok-plain.gguf")
    write(work / "tok-ds.gguf", [("w", [32], "F32", np.ones(32, np.float32).view(np.uint8))], tokenizer_kv("deepseek3"))
    ok &= compare_tokenizer("tokenizer-deepseek3", work / "tok-ds.gguf")

    for i, real in enumerate(A.maya_real):
        ok &= compare_maya(f"maya-real-{i}", pathlib.Path(real), [], setup=True)

    # DeepSeek-V4.1 (tools/ds4/pack_gguf.py): the Engram tables three ways
    for variant in ("engram", "newer", "ple"):
        m, kv = ds4_model(variant=variant)
        model = work / f"ds4-{variant}" / "DeepSeek-V4.1-Test"
        shutil.rmtree(model, ignore_errors=True)
        model.mkdir(parents=True)
        write(model / "ds4.gguf", m.tensors, kv, "deepseek41")
        ok &= compare_moe(f"ds4-{variant}", "ds4", model)
    # MiniMax-M3.1 (tools/mm3/pack_gguf.py): with its indexers side file, then without it (both refuse)
    m, side, kv = mm3_model()
    model = work / "mm3" / "MiniMax-M3.1-Test"
    shutil.rmtree(model, ignore_errors=True)
    model.mkdir(parents=True)
    write(model / "mm3.gguf", m.tensors, kv, "minimax-m3")
    write(model / "mm3-maya-indexers.gguf", side.tensors, {"maya.indexers.for": ("string", "MiniMax-M3.1-Test")}, "minimax-m3")
    ok &= compare_moe("mm3", "mm3", model)
    (model / "mm3-maya-indexers.gguf").unlink()
    ok &= compare_moe("mm3-no-indexers", "mm3", model)
    return ok


def raon_cases(work: pathlib.Path) -> bool:
    """raon1 (Raon-1-mini): our additions to Maya's iq_pack.py and strata_tokenizer.py (--raon-tools) against
    `strata-prepare pack --maya` / `download --maya` / `tokenizer`."""
    tools = A.raon_tools
    ok = True
    m, kv = raon1_model()
    write(work / "raon1.gguf", m.tensors, kv, "raon1")
    ok &= compare_maya("raon1-compat-bf16", work / "raon1.gguf", ["--compat-bf16"], tools=tools)
    ok &= compare_maya("raon1-plain", work / "raon1.gguf", [], tools=tools)
    ok &= compare_maya("raon1-experts-bin", work / "raon1.gguf", ["--compat-bf16", "--experts-bin"], tools=tools)
    ok &= compare_maya("raon1-setup", work / "raon1.gguf", [], setup=True, tools=tools)
    ok &= compare_tokenizer("tokenizer-raon", work / "raon1.gguf", tools=tools)
    # the IQ4_XS build's layout: i-quant experts, attn_k_b / attn_v_b quantized (to F32 in dense.bin), two leading blocks
    m, kv = raon1_model(seed=42, blocks=4, lead=2, gu="IQ4_XS", down="IQ4_NL", quantized_kb=True)
    write(work / "raon1-iq4.gguf", m.tensors, kv, "raon1")
    ok &= compare_maya("raon1-iq4", work / "raon1-iq4.gguf", ["--compat-bf16"], tools=tools)
    ok &= compare_maya("raon1-iq4-setup", work / "raon1-iq4.gguf", [], setup=True, tools=tools)
    for i, real in enumerate(A.raon_real):
        ok &= compare_maya(f"raon-real-{i}", pathlib.Path(real), [], setup=True, tools=tools)
    return ok


def main() -> int:
    global A, Q, GGUFWriter, GGML_QUANT_SIZES
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--tools")
    ap.add_argument("--maya-tools")
    ap.add_argument("--raon-tools")
    ap.add_argument("--gguf-py", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--real", action="append", default=[])
    ap.add_argument("--real-compat", action="store_true")
    ap.add_argument("--maya-real", action="append", default=[])
    ap.add_argument("--raon-real", action="append", default=[])
    A = ap.parse_args()
    if not A.tools and not A.maya_tools and not A.raon_tools:
        ap.error("--tools, --maya-tools, --raon-tools or any of them")
    sys.path.insert(0, A.gguf_py)
    import gguf  # noqa: E402
    Q, GGUFWriter, GGML_QUANT_SIZES = gguf.GGMLQuantizationType, gguf.GGUFWriter, gguf.GGML_QUANT_SIZES
    work = pathlib.Path(A.work)
    work.mkdir(parents=True, exist_ok=True)
    ok = True
    if A.tools:
        ok &= upstream_cases(work)
    if A.maya_tools:
        ok &= maya_cases(work)
    if A.raon_tools:
        ok &= raon_cases(work)
    print("PARITY OK" if ok else "PARITY FAILED", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
