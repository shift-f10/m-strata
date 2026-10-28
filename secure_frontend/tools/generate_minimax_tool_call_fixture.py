"""Generate tests/fixtures/minimax_tool_calls.json - how Project Maya reads tool calls out of a MiniMax-M3.1 reply.

The reference is Maya v1.1.30's serve/frontend.py OutputParser as its Service.run builds it for a MiniMax chat
(serve/server.py: thinking, the request's tools, stream_tools=True, minimax=True - its MinimaxRewriter turns MiniMax's
namespaced calls into GLM's form, string parameters by the tool's schema JSON-quoted as they stream, other values whole as
the JSON they spell, read with no schema), imported from a byte-exact copy of Maya's serve folder. Each reply is fed whole
and again one character at a time, and both must give the same events (asserted); the events are written merged -
consecutive reasoning or content texts joined, empty ones dropped, the streaming tool_start / tool_args pieces left out -
with each call's name and arguments. A reply is the model's text with `</mm:think>` where the thinking ends (our engine
run splits the reasoning at that special token).

Usage: python -I tools/generate_minimax_tool_call_fixture.py <Maya serve dir's parent> > tests/fixtures/minimax_tool_calls.json
"""
import json
import pathlib
import sys

TOOL = {"name": "web_search", "description": "Search the web.",
        "parameters": {"type": "object",
                       "properties": {"query": {"type": "string"}, "max_results": {"type": "integer"},
                                      "note": {"type": ["string", "null"]}},
                       "required": ["query"]}}
NS = "]<]minimax[>["
W = chr(0x3000)  # whitespace beyond ASCII, which Python's str.split and str.strip take as whitespace


def arg(name, value):
    return f"{NS}<{name}>{value}{NS}</{name}>"


def invoke(*args, name="web_search"):
    return f'{NS}<invoke name="{name}">' + "".join(args) + f"{NS}</invoke>\n"


def calls(*invokes):
    return f"{NS}<tool_call>\n" + "".join(invokes) + f"{NS}</tool_call>"


CASES = [
    ("a_call_after_the_thinking", "I need to search.</mm:think>" + calls(invoke(arg("query", "world cup 2026")))),
    ("content_before_a_call", "ok</mm:think>Let me check.\n" + calls(invoke(arg("query", "x"), arg("max_results", "3")))),
    ("two_invokes", "x</mm:think>" + calls(invoke(arg("query", "a")), invoke(arg("query", "b")))),
    ("nested_values", "x</mm:think>" + calls(invoke(arg("query", "q"), arg("opts", arg("k", "v") + arg("n", "1")),
                                                     arg("tags", arg("item", "a") + arg("item", "b")),
                                                     arg("flag", "true"), arg("ratio", "2.50")))),
    ("a_string_parameter_stays_a_string", "x</mm:think>" + calls(invoke(arg("query", "[1, 2]"), arg("note", "42")))),
    ("an_untyped_value_is_its_json_or_its_text", "x</mm:think>" + calls(invoke(arg("query", "q"), arg("n", "7"), arg("word", "plain text"),
                                                                                arg("none", "null")))),
    ("a_string_keeps_newlines_angles_and_tags",
     "x</mm:think>" + calls(invoke(arg("query", "\nline <one>\na < b " + NS + "<inner>z" + NS + "</inner>\n")))),
    ("text_after_the_calls", "x</mm:think>" + calls(invoke(arg("query", "q"))) + "\n\nDone."),
    ("an_unclosed_call_is_closed_at_the_end", "x</mm:think>" + f"{NS}<tool_call>\n{NS}<invoke name=\"web_search\">" + arg("query", "q")),
    ("a_cut_value_at_the_end", "x</mm:think>" + f"{NS}<tool_call>\n{NS}<invoke name=\"web_search\">{NS}<query>half"),
    ("a_call_in_the_reasoning_is_reasoning", "Like " + calls(invoke(arg("query", "q"))) + " then.</mm:think>Answer."),
    ("the_namespace_alone_is_text", "x</mm:think>The token " + NS + " alone, and " + NS + "<b>x" + NS + "</b>."),
    ("unicode_around_a_call", "思考。</mm:think>答え：" + calls(invoke(arg("query", "日本 été")))),
    ("python_whitespace_in_tags_and_values",
     "x</mm:think>" + f"{NS}<tool_call>\n{NS}<invoke{W}name=\"web_search\">" + arg("query", "q") + arg("max_results", f" 5{W}") +
     f"{NS}</invoke>\n{NS}</tool_call>"),
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
        p = OutputParser(thinking=True, tools=[TOOL], stream_tools=True, minimax=True)
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
    out = {"generator": "tools/generate_minimax_tool_call_fixture.py", "tool": TOOL, "cases": cases}
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
