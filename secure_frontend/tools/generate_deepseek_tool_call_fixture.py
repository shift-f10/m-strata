"""Generate tests/fixtures/deepseek_tool_calls.json - how Project Maya reads tool calls out of a DeepSeek-V4.1 reply.

The reference is Maya v1.1.30's serve/frontend.py OutputParser as its Service.run builds it for a DeepSeek chat
(serve/server.py: thinking, the request's tools, stream_tools=True, dsml=True - its DsmlRewriter turns DeepSeek's DSML
calls into GLM's form, read with no schema, so every value is the JSON it holds), imported from a byte-exact copy of
Maya's serve folder. Each reply is fed whole and again one character at a time, and both must give the same events
(asserted); the events are written merged - consecutive reasoning or content texts joined, empty ones dropped, the
streaming tool_start / tool_args pieces left out - with each call's name and arguments. A reply is the model's text with
`</think>` where the thinking ends (｜DSML｜ is a special token the engine outputs as its text).

Usage: python -I tools/generate_deepseek_tool_call_fixture.py <Maya serve dir's parent> > tests/fixtures/deepseek_tool_calls.json
"""
import json
import pathlib
import sys

TOOL = {"name": "web_search", "description": "Search the web.",
        "parameters": {"type": "object",
                       "properties": {"query": {"type": "string"}, "max_results": {"type": "integer"}},
                       "required": ["query"]}}
D = "｜DSML｜"
W, NBSP = chr(0x3000), chr(0xA0)  # whitespace beyond ASCII, which Python's str.split and str.strip take as whitespace


def param(name, value, string=True):
    return f'<{D} parameter name="{name}" string="{"true" if string else "false"}">{value}</{D} parameter>'


def invoke(*params, name="web_search"):
    return f'<{D} invoke name="{name}">\n' + "\n".join(params) + f"\n</{D} invoke>"


def calls(*invokes):
    return f"\n\n<{D} calls>\n" + "\n".join(invokes) + f"\n</{D} calls>"


CASES = [
    ("a_call_after_the_thinking", "I need to search.</think>" + calls(invoke(param("query", "world cup 2026")))),
    ("content_before_a_call", "ok</think>Let me check." + calls(invoke(param("query", "x"), param("max_results", "3", False)))),
    ("two_calls_in_one_block", "x</think>" + calls(invoke(param("query", "a")), invoke(param("query", "b")))),
    ("json_values", "x</think>" + calls(invoke(param("query", "q"), param("max_results", "5", False),
                                                   param("list", '[1, 2.5, "z"]', False), param("flag", "true", False),
                                                   param("opts", '{"k": "v"}', False)))),
    ("a_string_that_looks_like_json_stays_a_string", "x</think>" + calls(invoke(param("query", "[1, 2]"), param("n", "7")))),
    ("a_value_keeps_its_newlines_and_angles", "x</think>" + calls(invoke(param("query", "\nline <one>\na < b\n")))),
    ("a_dsml_tag_inside_a_value_is_data",
     "x</think>" + calls(invoke(param("query", f"see <{D} invoke name=\"y\"> and </{D} para")))),
    ("text_after_the_calls", "x</think>" + calls(invoke(param("query", "q"))) + "\n\nDone."),
    ("an_unclosed_block_still_gives_its_call",
     "x</think>" + f"\n\n<{D} calls>\n<{D} invoke name=\"web_search\">\n" + param("query", "q") + f"\n</{D} invoke>"),
    ("a_cut_value_at_the_end",
     "x</think>" + f"\n\n<{D} calls>\n<{D} invoke name=\"web_search\">\n<{D} parameter name=\"query\" string=\"true\">half"),
    ("a_call_in_the_reasoning_is_reasoning", "Like " + calls(invoke(param("query", "q"))) + " then.</think>Answer."),
    ("plain_angles_are_text", "x</think>If a < b and c > d, then <b>bold</b>."),
    ("unicode_around_a_call", "思考。</think>答え：" + calls(invoke(param("query", "日本 été")))),
    ("python_whitespace_in_the_tags",
     "x</think>" + f"\n\n<{D}{W}calls>\n<{D}{NBSP}invoke name=\"web_search\">{W}\n" + param("query", "q") +
     f"\n</{D}{W}invoke>\n</{D} calls>"),
]


def merged(events):
    out = []
    for e in events:
        if e.kind == "tool_call":
            out.append({"call": {"name": e.call.name, "arguments": e.call.arguments}})
        elif e.kind in ("reasoning", "content") and e.text:
            if out and out[-1].get("kind") == e.kind:
                out[-1]["text"] += e.text
            else:
                out.append({"kind": e.kind, "text": e.text})
    return out


def main():
    sys.path.insert(0, str(pathlib.Path(sys.argv[1])))
    from serve.frontend import OutputParser          # noqa: E402 - Maya's own parser

    def parse(text, step):
        p = OutputParser(thinking=True, tools=[TOOL], stream_tools=True, dsml=True)
        events = []
        for i in range(0, len(text), step):
            events += p.feed(text[i:i + step])
        events += p.finish()
        return merged(events)

    cases = []
    for name, text in CASES:
        whole = parse(text, max(1, len(text)))
        assert parse(text, 1) == whole, name
        cases.append({"name": name, "reply": text, "events": whole})
    out = {"generator": "tools/generate_deepseek_tool_call_fixture.py", "tool": TOOL, "cases": cases}
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
