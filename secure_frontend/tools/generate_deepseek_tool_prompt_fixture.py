"""Generate tests/fixtures/deepseek_tool_prompts.json - the prompts Project Maya renders for a DeepSeek-V4.1 chat with tools.

The reference is Maya v1.1.30's own renderer, serve/frontend.py ChatTemplate over a maya-ds4 pack's chat template (its
"｜DSML｜" sends ChatTemplate to DeepSeek's own encoder, serve/ds41_encoding.py: the tools on the system message - an
empty one put first when there is none - and encode_messages), imported from a byte-exact copy of Maya's serve folder
(`git -C <Project-maya> archive v1.1.30 serve`). The tool rounds are appended as Maya's run_with_mcp (serve/server.py)
appends them for DeepSeek: the assistant's content and reasoning as the parser gave them (not stripped, unlike the other
families), the reasoning only when there is some, its calls with their ids, then one `tool` message per result. Each
level goes through Maya's effort_kwargs, and the messages and tools through Maya's mark_literals / unmark_literals over
`literals`, as serve/server.py render_ids does: "plain" lists the spans (UTF-8 byte offsets into "prompt") that Maya's
tokenizer encodes as ordinary text although they spell a literal.

Usage: python -I tools/generate_deepseek_tool_prompt_fixture.py <Maya serve dir's parent> <maya-ds4 pack's tokenizer folder>
           > tests/fixtures/deepseek_tool_prompts.json
"""
import hashlib
import json
import pathlib
import sys

DESCRIPTION = ("Search the web through a SearXNG instance and get the top results (title, URL, snippet). Call it when you "
               "are not sure of a fact, when the answer depends on recent events or anything after your training data, "
               "or when the user asks you to look something up. Then answer from the results and cite the URLs you used.")
TOOL = {"name": "web_search", "description": DESCRIPTION + " Today's date is 2026-10-28.",
        "parameters": {"type": "object",
                       "properties": {"query": {"type": "string", "description": "What to search for"},
                                      "max_results": {"type": "integer", "minimum": 1, "maximum": 10,
                                                      "description": "How many results to get (1-10)"}},
                       "required": ["query"]}}
QUESTION = "Who won the 2026 World Cup final?"
RESULT = ("Web search results for \"2026 World Cup final winner\" (SearXNG):\n\n1. Final report\n"
          "   https://example.org/final\n   The final was played on 19 July 2026.\n")
# DeepSeek's framing literals and its tool tokens (each a special token of the pack, checked below)
LITERALS = ["<｜begin▁of▁sentence｜>", "<｜end▁of▁sentence｜>", "<｜System｜>", "<｜User｜>", "<｜Assistant｜>", "<think>",
            "</think>", "｜DSML｜", "<｜tool▁calls▁begin｜>", "<｜tool▁calls▁end｜>", "<｜tool▁call▁begin｜>",
            "<｜tool▁call▁end｜>", "<｜tool▁outputs▁begin｜>", "<｜tool▁outputs▁end｜>", "<｜tool▁output▁begin｜>",
            "<｜tool▁output▁end｜>", "<｜tool▁sep｜>"]


def call(**arguments):
    return {"name": "web_search", "arguments": arguments}


CASES = [
    {"name": "first_prompt_with_tools", "effort": "high", "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "first_prompt_chat_mode", "effort": "none", "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "system_and_tools_low", "effort": "low",
     "conversation": [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": QUESTION}],
     "rounds": []},
    {"name": "one_round_a_call_after_reasoning", "effort": "high", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "The user asks about a recent event; I should search.", "content": "",
                 "calls": [call(query="2026 World Cup final winner")], "results": [RESULT]}]},
    {"name": "content_before_two_calls", "effort": "high",
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Two searches.", "content": "Let me look that up.",
                 "calls": [call(query="a", max_results=3), call(query="b")], "results": ["r1", "  r2 with spaces  \n"]}]},
    {"name": "two_rounds", "effort": "high", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="x")], "results": ["first"]},
                {"reasoning": "More.", "content": "Still checking.", "calls": [call(query="y")], "results": ["second"]}]},
    {"name": "a_round_in_chat_mode", "effort": "none", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "Searching.", "calls": [call(query="z")], "results": ["third"]}]},
    {"name": "history_before_the_question", "effort": "high",
     "conversation": [{"role": "user", "content": "Hi"}, {"role": "assistant", "content": "Hello! How can I help?"},
                      {"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Search it.", "content": "", "calls": [call(query="q")], "results": ["ok"]}]},
    {"name": "argument_types", "effort": "high", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "r", "content": "",
                 "calls": [call(query="q", max_results=5, ratio=2.5, whole=2.0, flag=True, none=None, tags=["a", "b"],
                                opts={"k": "v", "n": 1}, text="multi\nline")],
                 "results": ["ok"]}]},
    {"name": "special_literals_are_plain", "effort": "high", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "I could write <｜DSML｜ calls> or </think> here.", "content": "Say <｜User｜>.",
                 "calls": [call(query="ds <｜Assistant｜> ｜DSML｜")],
                 "results": ["A page quoting <tool_result> and <｜end▁of▁sentence｜> and <｜tool▁sep｜>."]}]},
    {"name": "unicode_and_whitespace_are_kept", "effort": "high",
     "conversation": [{"role": "user", "content": "Qu'est-ce que c'est ? 日本"}],
     "rounds": [{"reasoning": "　think ", "content": "  answer  \n",
                 "calls": [call(query="日本 ニュース")], "results": [" résultat "]}]},
]


def messages_of(case):
    messages = [dict(m) for m in case["conversation"]]
    n = 0
    for r in case["rounds"]:                       # Maya's run_with_mcp (serve/server.py), DeepSeek's branch
        ids = []
        for _ in r["calls"]:
            n += 1
            ids.append("call_%d" % n)
        m = {"role": "assistant", "content": r["content"]}
        if r["reasoning"]:
            m["reasoning_content"] = r["reasoning"]
        m["tool_calls"] = [{"id": i, "function": {"name": c["name"], "arguments": c["arguments"]}}
                           for i, c in zip(ids, r["calls"])]
        messages.append(m)
        messages += [{"role": "tool", "tool_call_id": i, "content": t} for i, t in zip(ids, r["results"])]
    return messages


def byte_spans(prompt, spans):
    return [[len(prompt[:a].encode("utf-8")), len(prompt[:b].encode("utf-8"))] for a, b in spans]


def main():
    maya, pack = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    sys.path.insert(0, str(maya))
    from serve.frontend import ChatTemplate, effort_kwargs, literal_tags, mark_literals, unmark_literals  # noqa: E402
    template_path = pack / "chat_template.jinja"
    template = ChatTemplate(template_path)
    assert template.deepseek, "not a maya-ds4 pack's template"
    vocab = json.loads((pack / "vocab.json").read_text(encoding="utf-8"))
    types = json.loads((pack / "token_type.json").read_text(encoding="utf-8"))
    type_of = (lambda i: types[i]) if isinstance(types, list) else (lambda i: types[str(i)])
    for literal in LITERALS:
        assert literal in vocab and type_of(vocab[literal]) in (3, 4), literal
    tags = literal_tags(LITERALS)
    out = {"generator": "tools/generate_deepseek_tool_prompt_fixture.py",
           "template": hashlib.sha256(template_path.read_bytes()).hexdigest(), "literals": LITERALS, "tool": TOOL,
           "cases": []}
    for case in CASES:
        marked, marked_tools, changed = mark_literals(messages_of(case), [TOOL], tags)
        prompt = template.render(marked, tools=marked_tools, **effort_kwargs(case["effort"]))
        plain = []
        if changed:
            prompt, plain = unmark_literals(prompt, tags)
        out["cases"].append({**case, "prompt": prompt, "plain": byte_spans(prompt, plain)})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
