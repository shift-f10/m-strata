"""Generate tests/fixtures/qwen_tool_calls.json - how upstream Strata reads tool calls out of a Qwen reply.

The reference is upstream's serve/frontend.py OutputParser, as run_with_mcp drives it (Service.run with the request's
tools and declared_only=True; recover off, the config's default; stream_tools off): each reply is fed whole and again
one character at a time, and both must give the same events (asserted), which are written merged - consecutive
reasoning or content texts joined, empty ones dropped - with each call's name and arguments.

A reply is the model's text with `</think>` where the thinking ends (never quoted, #537, so it is the end the engine's
token gives our generation run); `finish` is how the turn ended ("stop" or "length").

Usage: python -I tools/generate_qwen_tool_call_fixture.py <upstream dir> > tests/fixtures/qwen_tool_calls.json
"""
import json
import pathlib
import sys

TOOL = {"name": "web_search", "description": "Search the web.",
        "parameters": {"type": "object",
                       "properties": {"query": {"type": "string"}, "max_results": {"type": "integer"}},
                       "required": ["query"]}}


def call(query, extra=""):
    return ("<tool_call>\n<function=web_search>\n<parameter=query>\n" + query + "\n</parameter>\n" + extra +
            "</function>\n</tool_call>")


CASES = [
    ("a_call_after_the_thinking", True, "I need to search.\n</think>\n\n" + call("world cup 2026"), "stop"),
    ("content_before_a_call", True,
     "ok</think>\n\nLet me check.\n\n" + call("x", "<parameter=max_results>\n3\n</parameter>\n"), "stop"),
    ("a_call_in_the_reasoning_then_the_end_of_it", True, "I will search.\n" + call("q") + "\n</think>\n\n", "stop"),
    ("a_call_in_the_reasoning_then_text_is_a_quote", True,
     "Like this:\n" + call("q") + "\nBut I won't.\n</think>\n\nAnswer.", "stop"),
    ("a_call_mid_sentence_in_the_reasoning_is_text", True, "I would write " + call("q") + " here.</think>\n\nNo.", "stop"),
    ("a_call_in_the_reasoning_at_a_natural_end", True, "Search:\n" + call("q"), "stop"),
    ("a_call_in_the_reasoning_cut_by_length", True, "Search:\n" + call("q"), "length"),
    ("inline_code_in_the_reasoning_is_a_quote", True, "Use `<tool_call>` like so\n</think>\n\nok", "stop"),
    ("a_fenced_call_in_the_answer_is_text", True,
     "x</think>\n\n```\n" + call("q") + "\n```\nThat is the format.", "stop"),
    ("an_undeclared_tool_is_text", True,
     "x</think>\n\n<tool_call>\n<function=other_tool>\n<parameter=a>\n1\n</parameter>\n</function>\n</tool_call>", "stop"),
    ("prose_naming_the_tag_is_text", True, "x</think>\n\nI'll use a <tool_call> block soon.", "stop"),
    ("a_drifted_opener_names_the_tool", True,
     "x</think>\n\n<tool_call>\n<web_search>\n<parameter=query>\nq\n</parameter>\n</function>\n</tool_call>", "stop"),
    ("a_spaced_opener_names_the_tool", True,
     "x</think>\n\n<tool_call>\n<function =web_search>\n<parameter=query>\nq\n</parameter>\n</function>\n</tool_call>",
     "stop"),
    ("spaces_around_a_declared_name", True,
     "x</think>\n\n<tool_call>\n<function= web_search >\n<parameter=query>\nq\n</parameter>\n</function>\n</tool_call>",
     "stop"),
    ("two_calls_and_text_after", True, "x</think>\n\n" + call("a") + "\n" + call("b") + "\n\nDone.", "stop"),
    ("a_value_holding_the_closing_tags", True,
     "x</think>\n\n" + call("a </parameter> b </tool_call> c </function> d"), "stop"),
    ("an_unterminated_call_is_text", True,
     "x</think>\n\n<tool_call>\n<function=web_search>\n<parameter=query>\nq", "stop"),
    ("thinking_off_text_around_a_call", False, "Sure.\n" + call("q") + "\nAfter text.", "stop"),
    ("an_omitted_closer_before_the_next_call", True,
     "x</think>\n\n<tool_call>\n<function=web_search>\n<parameter=query>\na\n</parameter>\n</function>\n" + call("b"),
     "stop"),
    ("values_are_decoded_unless_strings", True,
     "x</think>\n\n" + call("5", "<parameter=max_results>\n5\n</parameter>\n<parameter=list>\n[1, 2.5, \"z\"]\n"
                                 "</parameter>\n<parameter=word>\nnotjson\n</parameter>\n<parameter=flag>\ntrue\n"
                                 "</parameter>\n<parameter=nothing>\nnull\n</parameter>\n"), "stop"),
    ("newlines_before_a_call_are_dropped", True, "x</think>\n\nA\n\n\n" + call("q") + "\n\n\nB", "stop"),
    ("a_partial_tag_at_the_end_is_text", True, "x</think>\n\nThe end <tool_", "stop"),
    ("a_second_call_in_the_reasoning_waits_too", True,
     "Two:\n" + call("a") + "\n\n" + call("b") + "\n</think>\n\nThanks.", "stop"),
    ("a_call_in_a_reasoning_fence_is_a_quote", True, "Format:\n```xml\n" + call("q") + "\n```\nok\n</think>\n\nHi.", "stop"),
    ("a_duplicate_parameter_keeps_its_first_place", True,
     "x</think>\n\n<tool_call>\n<function=web_search>\n<parameter=query>\na\n</parameter>\n<parameter=max_results>\n2\n"
     "</parameter>\n<parameter=query>\nb\n</parameter>\n</function>\n</tool_call>", "stop"),
    ("unicode_around_a_call", True, "思考。\n</think>\n\n答え：\n" + call("日本 été"), "stop"),
]


def merged(events):
    out = []
    for e in events:
        if e.kind == "tool_call":
            out.append({"call": {"name": e.call.name, "arguments": e.call.arguments}})
        elif e.text:
            if out and out[-1].get("kind") == e.kind:
                out[-1]["text"] += e.text
            else:
                out.append({"kind": e.kind, "text": e.text})
    return out


def main():
    sys.path.insert(0, str(pathlib.Path(sys.argv[1])))
    from serve.frontend import OutputParser          # noqa: E402 - upstream's own parser

    def parse(thinking, text, finish, step):
        p = OutputParser(thinking=thinking, tools=[TOOL], declared_only=True)
        events = []
        for i in range(0, len(text), step):
            events += p.feed(text[i:i + step])
        events += p.finish(finish)
        return merged(events)

    cases = []
    for name, thinking, text, finish in CASES:
        whole = parse(thinking, text, finish, max(1, len(text)))
        assert parse(thinking, text, finish, 1) == whole, name
        cases.append({"name": name, "thinking": thinking, "reply": text, "finish": finish, "events": whole})
    out = {"generator": "tools/generate_qwen_tool_call_fixture.py", "tool": TOOL, "cases": cases}
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
