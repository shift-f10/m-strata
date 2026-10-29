"""Generate tests/fixtures/minimax_tool_prompts.json - the prompts Project Maya renders for a MiniMax-M3.1 chat with tools.

The reference is Maya v1.1.30's own renderer, serve/frontend.py ChatTemplate (Jinja with transformers' settings), over
Maya's MiniMax chat template (tools/mm3/chat_template.jinja: MiniMax's own plus thinking_mode "disabled", which every
maya-mm3 pack carries), imported from a byte-exact copy of Maya's serve folder (`git -C <Project-maya> archive v1.1.30
serve`). The tools go in Maya's flat form (ChatTemplate turns them into OpenAI's). The tool rounds are appended as Maya's
run_with_mcp (serve/server.py) appends them for a family other than DeepSeek: the assistant's content and reasoning
(each joined, then stripped; the reasoning only when there is some), its calls with their ids, then one `tool` message
per result. An earlier answer that searched (a conversation message with "rounds", as our page sends it in
strata_rounds; owner 2026-10-29, "keep earlier searches in the chat history as upstream's page does") goes in as
upstream's page sends it (serve/web/app.js assistantMessages) and its server normalizes it (serve/frontend.py
openai_to_messages, Maya's alike): per round an assistant message with the text before its calls and the calls (ids
dropped, arguments a mapping), one `tool` message per result, then the rest of its text when there is some. Each level
goes through Maya's effort_kwargs, and the messages and tools through Maya's mark_literals /
unmark_literals over `literals` (the template's five and its tool namespace), as serve/server.py render_ids does: "plain"
lists the spans (UTF-8 byte offsets into "prompt") that Maya's tokenizer encodes as ordinary text although they spell a
literal.

Usage: python -I tools/generate_minimax_tool_prompt_fixture.py <Maya serve dir's parent> <Maya's MiniMax chat template>
           > tests/fixtures/minimax_tool_prompts.json
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
NS = "]<]minimax[>["
# The template's five literals, its tool namespace, and the call tags it writes after the namespace - the special tokens
# of MiniMax-M3.1's vocabulary that these prompts spell (Maya marks every special token the vocabulary has; no other
# appears in them).
LITERALS = ["]~!b[", "]~b]", "[e~[", "<mm:think>", "</mm:think>", NS, "<tool_call>", "</tool_call>"]


def call(**arguments):
    return {"name": "web_search", "arguments": arguments}


CASES = [
    {"name": "first_prompt_with_tools", "effort": None, "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "first_prompt_thinking_off", "effort": "none", "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "system_and_tools_high", "effort": "high",
     "conversation": [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": QUESTION}],
     "rounds": []},
    {"name": "one_round_a_call_after_reasoning", "effort": None, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "\nThe user asks about a recent event; I should search.\n", "content": "",
                 "calls": [call(query="2026 World Cup final winner")], "results": [RESULT]}]},
    {"name": "content_before_two_calls", "effort": "low",
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Two searches.", "content": "Let me look that up.\n",
                 "calls": [call(query="a", max_results=3), call(query="b")], "results": ["r1", "  r2 with spaces  \n"]}]},
    {"name": "two_rounds", "effort": None, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="x")], "results": ["first"]},
                {"reasoning": "More.", "content": "Still checking.", "calls": [call(query="y")], "results": ["second"]}]},
    {"name": "a_round_with_thinking_off", "effort": "none", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "Searching.", "calls": [call(query="z")], "results": ["third"]}]},
    {"name": "history_before_the_question", "effort": None,
     "conversation": [{"role": "user", "content": "Hi"}, {"role": "assistant", "content": "Hello! How can I help?"},
                      {"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Search it.", "content": "", "calls": [call(query="q")], "results": ["ok"]}]},
    {"name": "argument_types", "effort": None, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "r", "content": "",
                 "calls": [call(query="q", max_results=5, ratio=2.5, whole=2.0, flag=True, off=False, none=None,
                                tags=["a", "b"], opts={"k": "v", "n": 1, "deep": {"x": [1, {"y": True}]}, "gone": None},
                                text="multi\nline")],
                 "results": ["ok"]}]},
    {"name": "special_literals_are_plain", "effort": None, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "I could write " + NS + "<invoke> or </mm:think> here.", "content": "Say ]~b]user.",
                 "calls": [call(query="mm " + NS + "<x> [e~[")],
                 "results": ["A page quoting <response> and ]~!b[ and " + NS + "</tool_call>."]}]},
    {"name": "unicode_whitespace_is_stripped", "effort": "medium",
     "conversation": [{"role": "user", "content": "Qu'est-ce que c'est ? 日本"}],
     "rounds": [{"reasoning": "　think ", "content": "  answer  ",
                 "calls": [call(query="日本 ニュース")], "results": [" résultat "]}]},
]


def searched(content, calls, results):
    """One round of an earlier answer, as our page sends it: the text before its calls, the calls, one result each."""
    return {"content": content, "calls": calls, "results": results}


def answer(content, *rounds):
    """An earlier answer that searched: its rounds, and `content` the text after the last one (may be empty)."""
    return {"role": "assistant", "content": content, "rounds": list(rounds)}


FOLLOW_UP = "Who scored the goals?"
FINAL = "Spain won it 2-1 ([report](https://example.org/final))."
# The chat history's searches (owner 2026-10-29).
CASES += [
    {"name": "history_one_round_one_call", "effort": None,
     "conversation": [{"role": "user", "content": QUESTION},
                      answer(FINAL, searched("", [call(query="2026 World Cup final winner")], [RESULT])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_two_rounds", "effort": "high",
     "conversation": [{"role": "user", "content": QUESTION},
                      answer(FINAL, searched("", [call(query="x")], ["first"]),
                             searched("Still checking.", [call(query="y", max_results=2)], ["second"])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_two_calls_in_a_round", "effort": "none",
     "conversation": [{"role": "user", "content": QUESTION},
                      answer("Both say Spain.",
                             searched("  Two searches at once.\n", [call(query="a", max_results=3), call(query="b")],
                                      ["r1", "  r2 with spaces  \n"])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_text_before_the_calls", "effort": "low",
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION},
                      answer("It was Spain.\n\nThey won 2-1.",
                             searched("Let me look that up.", [call(query="2026 World Cup final")], ["ok"])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_answer_with_empty_final_text", "effort": None,
     "conversation": [{"role": "user", "content": QUESTION},
                      answer("", searched("I'll check.", [call(query="q")], ["nothing found"])),
                      {"role": "user", "content": "Try again."}],
     "rounds": []},
    {"name": "history_then_this_turns_rounds", "effort": "medium",
     "conversation": [{"role": "user", "content": "Hi"}, {"role": "assistant", "content": "Hello! How can I help?"},
                      {"role": "user", "content": QUESTION},
                      answer(FINAL, searched("", [call(query="final")], [RESULT]),
                             searched("One more.", [call(query="score")], ["2-1"])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "Search the scorers.", "content": "", "calls": [call(query="2026 final goals")],
                 "results": ["Scorers: A, B, C."]},
                {"reasoning": "", "content": "Checking.", "calls": [call(query="c")], "results": ["third"]}]},
    {"name": "history_two_searched_answers", "effort": None,
     "conversation": [{"role": "user", "content": QUESTION},
                      answer("", searched("", [call(query="one")], ["1"])),
                      {"role": "user", "content": "And the year before?"},
                      answer("Nobody: there was none.", searched("Looking.", [call(query="two")], ["2"])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "r", "content": "", "calls": [call(query="three")], "results": ["3"]}]},
    {"name": "history_special_literals_are_plain", "effort": None,
     "conversation": [{"role": "user", "content": QUESTION},
                      answer("Done [e~[ and ]~!b[ here.",
                             searched("Say ]~b]user and <mm:think>.", [call(query="mm " + NS + "<x> [e~[")],
                                      ["A page quoting <response>, </mm:think> and ]~!b[ and " + NS + "</tool_call>."])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_argument_types", "effort": None,
     "conversation": [{"role": "user", "content": QUESTION},
                      answer(FINAL, searched("", [call(query="q", max_results=5, ratio=2.5, whole=2.0, flag=True,
                                                       off=False, none=None, tags=["a", "b"],
                                                       opts={"k": "v", "n": 1, "deep": {"x": [1, {"y": True}]},
                                                             "gone": None},
                                                       text="multi\nline")],
                                             ["ok"])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
]


def history_of(conversation):
    """The conversation as upstream's page sends it and upstream's (and Maya's) openai_to_messages normalizes it: an
    earlier answer that searched as its rounds' assistant and tool messages, then the rest of its text when not empty."""
    messages = []
    for m in conversation:
        if "rounds" not in m:
            messages.append(dict(m))
            continue
        for r in m["rounds"]:
            messages.append({"role": "assistant", "content": r["content"],
                             "tool_calls": [{"function": {"name": c["name"], "arguments": c["arguments"]}}
                                            for c in r["calls"]]})
            messages += [{"role": "tool", "content": t} for t in r["results"]]
        if m["content"]:
            messages.append({"role": "assistant", "content": m["content"]})
    return messages


def messages_of(case):
    messages = history_of(case["conversation"])
    n = 0
    for r in case["rounds"]:                       # Maya's run_with_mcp (serve/server.py), not DeepSeek: stripped
        ids = []
        for _ in r["calls"]:
            n += 1
            ids.append("call_%d" % n)
        m = {"role": "assistant", "content": r["content"].strip()}
        if r["reasoning"]:
            m["reasoning_content"] = r["reasoning"].strip()
        m["tool_calls"] = [{"id": i, "function": {"name": c["name"], "arguments": c["arguments"]}}
                           for i, c in zip(ids, r["calls"])]
        messages.append(m)
        messages += [{"role": "tool", "tool_call_id": i, "content": t} for i, t in zip(ids, r["results"])]
    return messages


def byte_spans(prompt, spans):
    return [[len(prompt[:a].encode("utf-8")), len(prompt[:b].encode("utf-8"))] for a, b in spans]


def main():
    maya, template_path = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    sys.path.insert(0, str(maya))
    from serve.frontend import ChatTemplate, effort_kwargs, literal_tags, mark_literals, unmark_literals  # noqa: E402
    template = ChatTemplate(template_path)
    assert template.minimax, "not Maya's MiniMax template"
    tags = literal_tags(LITERALS)
    out = {"generator": "tools/generate_minimax_tool_prompt_fixture.py",
           "template": hashlib.sha256(template_path.read_bytes()).hexdigest(), "literals": LITERALS, "tool": TOOL,
           "cases": []}
    for case in CASES:
        kwargs = effort_kwargs(case["effort"]) if case["effort"] is not None else {}
        marked, marked_tools, changed = mark_literals(messages_of(case), [TOOL], tags)
        prompt = template.render(marked, tools=marked_tools, **kwargs)
        plain = []
        if changed:
            prompt, plain = unmark_literals(prompt, tags)
        out["cases"].append({**case, "prompt": prompt, "plain": byte_spans(prompt, plain)})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
