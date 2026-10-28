"""Generate tests/fixtures/glm_tool_calls.json - how Project Maya reads tool calls out of a GLM-5.3 reply.

The reference is Maya v1.1.30's serve/frontend.py OutputParser as its Service.run builds it for a GLM chat
(serve/server.py: thinking, the request's tools, stream_tools=True, no DSML or MiniMax rewriting, strict_tools and
single_tool off), imported from a byte-exact copy of Maya's serve folder. Each reply is fed whole and again one character
at a time, and both must give the same events (asserted); the events are written merged - consecutive reasoning or
content texts joined, empty ones dropped, the streaming tool_start / tool_args pieces left out - with each call's name
and arguments. A reply is the model's text with `</think>` where the thinking ends.

Usage: python -I tools/generate_glm_tool_call_fixture.py <Maya serve dir's parent> > tests/fixtures/glm_tool_calls.json
"""
import json
import pathlib
import sys

TOOL = {"name": "web_search", "description": "Search the web.",
        "parameters": {"type": "object",
                       "properties": {"query": {"type": "string"}, "max_results": {"type": "integer"}},
                       "required": ["query"]}}


def call(query, extra=""):
    return "<tool_call>web_search<arg_key>query</arg_key><arg_value>" + query + "</arg_value>" + extra + "</tool_call>"


CASES = [
    ("a_call_after_the_thinking", "I need to search.</think>" + call("world cup 2026")),
    ("content_before_a_call", "ok</think>Let me check.\n\n" + call("x", "<arg_key>max_results</arg_key><arg_value>3</arg_value>")),
    ("glms_own_layout_with_newlines",
     "x</think><tool_call>web_search\n<arg_key>query</arg_key>\n<arg_value>rust 1.90</arg_value>\n<arg_key>max_results</arg_key>"
     "\n<arg_value>2</arg_value>\n</tool_call>"),
    ("a_call_in_the_reasoning_stays_reasoning", "Like this: " + call("q") + " then.</think>Answer."),
    ("a_fenced_call_in_the_answer_is_text", "x</think>```\n" + call("q") + "\n```\nThat is the format."),
    ("inline_code_is_text", "x</think>Write `" + call("q") + "` to search."),
    ("two_calls_and_text_after", "x</think>" + call("a") + "\n" + call("b") + "\n\nDone."),
    ("values_are_decoded_unless_strings",
     "x</think>" + call("5", "<arg_key>max_results</arg_key><arg_value>5</arg_value><arg_key>list</arg_key>"
                             "<arg_value>[1, 2.5, \"z\"]</arg_value><arg_key>word</arg_key><arg_value>notjson</arg_value>"
                             "<arg_key>flag</arg_key><arg_value>true</arg_value>")),
    ("a_value_keeps_its_newlines", "x</think>" + call("\nline one\nline two\n")),
    ("newlines_before_a_call_are_dropped", "x</think>A\n\n\n" + call("q") + "\n\n\nB"),
    ("a_partial_tag_at_the_end_is_text", "x</think>The end <tool_"),
    ("an_unfinished_call_with_its_name_is_a_call", "x</think><tool_call>web_search<arg_key>query</arg_key><arg_value>q"),
    ("an_unfinished_call_without_a_name_is_text", "x</think>Now <tool_call>web_se"),
    ("unicode_around_a_call", "思考。</think>答え：\n" + call("日本 été")),
    ("a_duplicate_key_keeps_its_first_place",
     "x</think><tool_call>web_search<arg_key>query</arg_key><arg_value>a</arg_value><arg_key>max_results</arg_key>"
     "<arg_value>2</arg_value><arg_key>query</arg_key><arg_value>b</arg_value></tool_call>"),
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
        p = OutputParser(thinking=True, tools=[TOOL], stream_tools=True)
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
    out = {"generator": "tools/generate_glm_tool_call_fixture.py", "tool": TOOL, "cases": cases}
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
