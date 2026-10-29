"""Generate tests/fixtures/deepseek_tool_prompts.json - the prompts Project Maya renders for a DeepSeek-V4.1 chat with tools.

The reference is Maya v1.1.30's own renderer, serve/frontend.py ChatTemplate over a maya-ds4 pack's chat template (its
"｜DSML｜" sends ChatTemplate to DeepSeek's own encoder, serve/ds41_encoding.py: the tools on the system message - an
empty one put first when there is none - and encode_messages), imported from a byte-exact copy of Maya's serve folder
(`git -C <Project-maya> archive v1.1.30 serve`). The tool rounds are appended as Maya's run_with_mcp (serve/server.py)
appends them for DeepSeek: the assistant's content and reasoning as the parser gave them (not stripped, unlike the other
families), the reasoning only when there is some, its calls with their ids, then one `tool` message per result. An
earlier answer's searches in the conversation (its "rounds", HISTORY_CASES) go in as upstream's page sends them and
upstream's server normalizes them (history_of). Each
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
# Earlier searches in the chat history (owner 2026-10-29, "keep earlier searches in the chat history as upstream's page
# does"): an earlier answer's "rounds" (the page's strata_rounds: per round the text before its calls, the calls, one
# result each) and its text after the last round ("content", empty when it wrote nothing after them).
FOLLOW_UP = "Who scored the goals?"
HISTORY_CASES = [
    {"name": "history_one_round_one_call", "effort": "high",
     "conversation": [{"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "Spain won it 2-1.",
                       "rounds": [{"content": "", "calls": [call(query="2026 World Cup final winner")], "results": [RESULT]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_two_rounds", "effort": "high",
     "conversation": [{"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "Spain, from both pages.",
                       "rounds": [{"content": "", "calls": [call(query="x")], "results": ["first"]},
                                  {"content": "", "calls": [call(query="y", max_results=2)], "results": ["second"]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_two_calls_in_a_round_chat_mode", "effort": "none",
     "conversation": [{"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "Both say Spain.",
                       "rounds": [{"content": "", "calls": [call(query="a", max_results=3), call(query="b")],
                                   "results": ["r1", "  r2 with spaces  \n"]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_text_before_the_calls", "effort": "low",
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "It was Spain.",
                       "rounds": [{"content": "Let me look that up.", "calls": [call(query="final")], "results": ["ok"]},
                                  {"content": "One more check.", "calls": [call(query="final score")], "results": [""]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_answer_with_empty_final_text", "effort": "high",
     "conversation": [{"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "",
                       "rounds": [{"content": "Spain won it, as this page says.", "calls": [call(query="final")],
                                   "results": ["ok"]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_empty_final_text_chat_mode", "effort": "none",
     "conversation": [{"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "",
                       "rounds": [{"content": "", "calls": [call(query="a")], "results": ["r1"]},
                                  {"content": "Spain.", "calls": [call(query="b"), call(query="c")], "results": ["r2", "r3"]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_then_this_turns_rounds", "effort": "high",
     "conversation": [{"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "Spain won it 2-1.",
                       "rounds": [{"content": "", "calls": [call(query="2026 World Cup final winner")], "results": [RESULT]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "Search the scorers.", "content": "", "calls": [call(query="final scorers")], "results": ["goals"]},
                {"reasoning": "", "content": "Checking.", "calls": [call(query="final report")], "results": ["report"]}]},
    {"name": "history_empty_final_text_then_this_turns_round", "effort": "high",
     "conversation": [{"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "",
                       "rounds": [{"content": "", "calls": [call(query="final")], "results": ["ok"]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "Again.", "content": "", "calls": [call(query="scorers")], "results": ["goals"]}]},
    {"name": "two_earlier_answers_searched", "effort": "high",
     "conversation": [{"role": "user", "content": "Hi"}, {"role": "assistant", "content": "Hello! How can I help?"},
                      {"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "",
                       "rounds": [{"content": "", "calls": [call(query="final")], "results": ["ok"]}]},
                      {"role": "user", "content": FOLLOW_UP},
                      {"role": "assistant", "content": "Two goals by Spain.",
                       "rounds": [{"content": "", "calls": [call(query="scorers", max_results=5)], "results": ["goals"]}]},
                      {"role": "user", "content": "Thanks!"}],
     "rounds": []},
    {"name": "history_literals_are_plain", "effort": "high",
     "conversation": [{"role": "user", "content": QUESTION},
                      {"role": "assistant", "content": "Spain.",
                       "rounds": [{"content": "Say <｜User｜> or </think>.", "calls": [call(query="ds <｜Assistant｜> ｜DSML｜")],
                                   "results": ["A page quoting <tool_result> and <｜end▁of▁sentence｜> and <｜tool▁sep｜>."]}]},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_unicode_and_whitespace_are_kept", "effort": "high",
     "conversation": [{"role": "user", "content": "Qu'est-ce que c'est ? 日本"},
                      {"role": "assistant", "content": "  日本  \n",
                       "rounds": [{"content": "　voilà ", "calls": [call(query="日本 ニュース", tags=["a", "b"], flag=True)],
                                   "results": [" résultat "]}]},
                      {"role": "user", "content": "Merci"}],
     "rounds": []},
]
CASES += HISTORY_CASES


def history_of(message):
    """An earlier answer with searches as upstream's page sends it (upstream/serve/web/app.js assistantMessages) and
    upstream's server normalizes it (upstream/serve/frontend.py openai_to_messages): per round an assistant message with
    the text before its calls and the calls (no ids), one `tool` message per result, then the answer's text after its
    last round only when there is some."""
    if message.get("role") != "assistant" or "rounds" not in message:
        return [dict(message)]
    out = []
    for r in message["rounds"]:
        out.append({"role": "assistant", "content": r["content"],
                    "tool_calls": [{"function": {"name": c["name"], "arguments": c["arguments"]}} for c in r["calls"]]})
        out += [{"role": "tool", "content": t} for t in r["results"]]
    if message["content"]:
        out.append({"role": "assistant", "content": message["content"]})
    return out


def messages_of(case):
    messages = [m for message in case["conversation"] for m in history_of(message)]
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
