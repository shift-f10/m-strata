"""tools/secure/spm_bpe.py - the reference encoder for Gemma 4's tokenizer (the `spm_bpe` mode).

Gemma 4 plan 1 Task 6 (spec §2.5, §6.2). A pure-Python mirror of the C++ `spm_bpe` mode in
secure_frontend/src/tokenizer_pack.cpp, written from llama.cpp at 3cf03257 (src/llama-vocab.cpp:
:528-537 the pre-tokenizer `[^\\n]+|[\\n]+`, :619-738 the BPE session with Gemma 4's newline rule at
:637-644 and the <0xXX> fallback at :718-730, :3385-3391 the U+2581 escape, :3713-3730 the decode).
It is the oracle Task 7 checks against llama.cpp's own tokenizer on the real vocabulary.

  encode: every U+0020 becomes U+2581; the text splits into runs of non-newlines and runs of
  newlines; a newline-only run that is a vocabulary token is that token; otherwise symbols are
  whole code points merged by the lowest merge rank first (ties: leftmost); a final symbol that is
  not a token becomes its UTF-8 bytes' <0xXX> tokens. No BOS, no space prefix. A CONTROL (3) or
  USER_DEFINED (4) literal in the text is refused, as the C++ encoder refuses it.
  decode: NORMAL (1) tokens with U+2581 as a space, BYTE (6) tokens as their byte; any other type
  is refused.

Stdlib only; reads a pack directory (vocab.json, merges.json, token_type.json), never a GGUF.
"""
from __future__ import annotations

import heapq
import json
import pathlib
import re

SPACE = "▁"
NORMAL, CONTROL, USER_DEFINED, BYTE = 1, 3, 4, 6
_BYTE_TOKEN = re.compile(r"<0x([0-9A-F]{2})>")


class Encoder:
    def __init__(self, tokens: list[str], merges: list[tuple[str, str]], token_types: list[int]):
        if len(tokens) != len(token_types):
            raise ValueError("one token type per token")
        self.tokens = list(tokens)
        self.types = list(token_types)
        self.ids: dict[str, int] = {}
        for i, token in enumerate(self.tokens):
            if token in self.ids:
                raise ValueError("duplicate token %d" % i)
            self.ids[token] = i
        self.ranks: dict[tuple[int, int], int] = {}
        for rank, (left, right) in enumerate(merges):
            if left in self.ids and right in self.ids:
                self.ranks.setdefault((self.ids[left], self.ids[right]), rank)
        self.byte_ids: dict[int, int] = {}
        for i, token in enumerate(self.tokens):
            match = _BYTE_TOKEN.fullmatch(token)
            if self.types[i] == BYTE and match:
                self.byte_ids[int(match.group(1), 16)] = i
        self.special = [t for t, ty in zip(self.tokens, self.types) if ty in (CONTROL, USER_DEFINED)]

    @classmethod
    def from_pack(cls, directory) -> "Encoder":
        d = pathlib.Path(directory)
        vocab = json.loads((d / "vocab.json").read_text(encoding="utf-8"))
        tokens = [""] * len(vocab)
        for token, i in vocab.items():
            tokens[i] = token
        merges = [tuple(pair) for pair in json.loads((d / "merges.json").read_text(encoding="utf-8"))]
        types = json.loads((d / "token_type.json").read_text(encoding="utf-8"))
        return cls(tokens, merges, types)

    def _encode_run(self, run: str, out: list[int]) -> None:
        if set(run) == {"\n"} and run in self.ids:  # llama.cpp :637-644
            out.append(self.ids[run])
            return
        start = list(range(len(run)))  # symbol i covers run[start[i]:start[i] + size[i]]
        size = [1] * len(run)
        prev = [i - 1 for i in range(len(run))]
        nxt = [i + 1 if i + 1 < len(run) else -1 for i in range(len(run))]
        heap: list[tuple[int, int, int, int, int]] = []

        def add(left: int, right: int) -> None:
            if left < 0 or right < 0:
                return
            lid = self.ids.get(run[start[left]:start[left] + size[left]])
            rid = self.ids.get(run[start[right]:start[right] + size[right]])
            if lid is None or rid is None:
                return
            rank = self.ranks.get((lid, rid))
            if rank is not None:
                heapq.heappush(heap, (rank, left, right, size[left], size[right]))

        for i in range(1, len(run)):
            add(i - 1, i)
        while heap:
            _, left, right, left_size, right_size = heapq.heappop(heap)
            if size[left] == 0 or size[right] == 0 or size[left] != left_size or size[right] != right_size:
                continue  # stale: a symbol changed since the pair was queued
            size[left] += size[right]
            size[right] = 0
            nxt[left] = nxt[right]
            if nxt[right] >= 0:
                prev[nxt[right]] = left
            add(prev[left], left)
            add(left, nxt[left])
        for i in range(len(run)):
            if size[i] == 0:
                continue
            piece = run[start[i]:start[i] + size[i]]
            token = self.ids.get(piece)
            if token is not None:
                if self.types[token] in (CONTROL, USER_DEFINED):
                    raise ValueError("text cannot be encoded by ordinary tokens")
                out.append(token)
                continue
            for byte in piece.encode("utf-8"):
                if byte not in self.byte_ids:
                    raise ValueError("no <0x%02X> byte token" % byte)
                out.append(self.byte_ids[byte])

    def encode(self, text: str) -> list[int]:
        for literal in self.special:
            if literal in text:
                raise ValueError("special token literal in text")
        normalized = text.replace(" ", SPACE)
        out: list[int] = []
        for run in re.findall(r"[^\n]+|\n+", normalized):
            self._encode_run(run, out)
        return out

    def decode(self, ids: list[int]) -> str:
        data = bytearray()
        for i in ids:
            kind = self.types[i]
            if kind == BYTE:
                match = _BYTE_TOKEN.fullmatch(self.tokens[i])
                if not match:
                    raise ValueError("a byte token must be spelled <0xXX>")
                data.append(int(match.group(1), 16))
            elif kind == NORMAL:
                data += self.tokens[i].replace(SPACE, " ").encode("utf-8")
            else:
                raise ValueError("token %d (type %d) is not ordinary output" % (i, kind))
        return data.decode("utf-8")
