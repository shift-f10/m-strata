"""GLM-5.3 (`glm4` / `chatglm-bpe`) pre-tokenizer regression tests.

These use a deliberately tiny byte-level vocabulary: no model weights or
network access are required, while BPE merges make a wrong split observable.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from secure_tokenizer import (GLM4_PATTERN, QWEN35_PATTERN, Tokenizer,  # noqa: E402
                              bytes_to_unicode, pre_tokenizer_pattern)


class GlmTokenizerTest(unittest.TestCase):
    def tokenizer(self, pre: str = "glm4") -> Tokenizer:
        tokens = list(bytes_to_unicode().values()) + ["12", "123", "45", "456", "123456", "<sop>"]
        return Tokenizer(tokens, ["1 2", "12 3", "4 5", "45 6", "123 456"],
                         [1] * (len(tokens) - 1) + [3], pre=pre)

    def test_three_digit_boundaries_are_observable_in_bpe_ids(self):
        tokenizer = self.tokenizer()
        self.assertEqual(tokenizer.encode("1234567"),
                         [tokenizer.ids["123"], tokenizer.ids["456"], tokenizer.ids["7"]])
        self.assertEqual(tokenizer.decode(tokenizer.encode("1234567")), "1234567")
        qwen = self.tokenizer("qwen35")
        self.assertEqual(qwen.encode("1234567"), [qwen.ids[str(i)] for i in range(1, 8)])

    def test_alias_has_identical_boundaries_and_lossless_round_trips(self):
        for text in ["Hello, world!", "I'm 1234567", "e\u0301", "日本語 🐈\n\n", "  a\t\r\nb"]:
            glm4 = self.tokenizer("glm4")
            alias = self.tokenizer("chatglm-bpe")
            self.assertEqual(glm4.encode(text), alias.encode(text))
            self.assertEqual(glm4.decode(glm4.encode(text)), text)

    def test_control_literals_remain_controlled_by_parse_special(self):
        tokenizer = self.tokenizer()
        self.assertEqual(tokenizer.encode("<sop>", parse_special=True), [tokenizer.ids["<sop>"]])
        self.assertNotEqual(tokenizer.encode("<sop>"), [tokenizer.ids["<sop>"]])

    def test_pattern_metadata_uses_declared_pre_tokenizer(self):
        self.assertEqual(pre_tokenizer_pattern("glm4")[0], GLM4_PATTERN)
        self.assertEqual(pre_tokenizer_pattern("chatglm-bpe")[0], GLM4_PATTERN)
        self.assertEqual(pre_tokenizer_pattern("qwen35")[0], QWEN35_PATTERN)


if __name__ == "__main__":
    unittest.main()
