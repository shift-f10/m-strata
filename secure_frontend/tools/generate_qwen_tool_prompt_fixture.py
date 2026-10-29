"""Generate tests/fixtures/qwen_tool_prompts.json - the prompts upstream Strata renders for a chat with tools.

The reference is upstream's own renderer, serve/frontend.py ChatTemplate (Jinja with transformers' settings), over:

  * "pack": the Qwen3.6 pack's tokenizer/chat_template.jinja (the GGUF's own template, which upstream's server renders
    for that pack: serve/server.py, `tpath / "chat_template.jinja"`), with enable_thinking;
  * "serve": upstream's serve/chat_template.jinja (its fallback; the Qwen3.8 GGUF's own template, which carries the
    reasoning-effort sentences our Qwen renderer writes), with reasoning_effort; "at_end" builds the prompt as
    Service.render_prompt does for effort_position "end" (#458): the history at the default effort, EFFORT_TURN, then
    the generation tail.

The tool rounds are appended to the conversation as run_with_mcp (serve/server.py) appends them: the assistant's
content and reasoning (each joined, then stripped), its tool calls, then one `tool` message per result.

An earlier answer that searched (owner 2026-10-29, "keep earlier searches in the chat history as upstream's page
does") is a conversation message with "rounds" (ChatMessage::rounds: per round its text before the calls, the calls,
one result each) and "content" (its text after the last round, which may be empty). It goes to the template as
upstream's page sends it (serve/web/app.js assistantMessages: per round {content: the text before the calls, trimmed,
tool_calls}, one `tool` message per result, then {content: the rest, trimmed} only when the rest is not empty) and
upstream's server normalizes it (serve/frontend.py openai_to_messages: {"function": {"name", "arguments": <dict>}},
the ids dropped). Those cases render with the pack's template: Qwen3.8's (serve) keeps a think block on every earlier
assistant turn (preserve_thinking undefined), which our renderer does not write for any history.

Usage: python -I tools/generate_qwen_tool_prompt_fixture.py <upstream dir> <pack chat_template.jinja>
           > tests/fixtures/qwen_tool_prompts.json
"""
import hashlib
import json
import pathlib
import sys

DESCRIPTION = ("Search the web through a SearXNG instance and get the top results (title, URL, snippet). Call it when you "
               "are not sure of a fact, when the answer depends on recent events or anything after your training data, "
               "or when the user asks you to look something up. Then answer from the results and cite the URLs you used.")
TOOL = {"name": "web_search", "description": DESCRIPTION + " Today's date is 2026-10-27.",
        "parameters": {"type": "object",
                       "properties": {"query": {"type": "string", "description": "What to search for"},
                                      "max_results": {"type": "integer", "minimum": 1, "maximum": 10,
                                                      "description": "How many results to get (1-10)"}},
                       "required": ["query"]}}
QUESTION = "Who won the 2026 World Cup final?"
RESULT = ("Web search results for \"2026 World Cup final winner\" (SearXNG):\n\n1. Final report\n"
          "   https://example.org/final\n   The final was played on 19 July 2026.\n")


def call(**arguments):
    return {"name": "web_search", "arguments": arguments}


ANSWER = "Spain won the final on 19 July 2026, beating England 2-1 [1].\n\n[1] https://example.org/final"
FOLLOW_UP = "Who scored the winning goal?"


def searched(content, *rounds):
    """An earlier answer that searched, as our page sends it: its rounds (strata_rounds) and its text after them."""
    return {"role": "assistant", "content": content,
            "rounds": [{"content": text, "calls": calls, "results": results} for text, calls, results in rounds]}


CASES = [
    {"name": "first_prompt_with_tools", "template": "pack", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "system_and_history_with_tools", "template": "pack", "thinking": True,
     "conversation": [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": "Hi"},
                      {"role": "assistant", "content": "Hello! How can I help?"},
                      {"role": "user", "content": "What is the latest Python release?"}],
     "rounds": []},
    {"name": "one_round_a_call_after_reasoning", "template": "pack", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "\nThe user asks about a recent event; I should search.\n", "content": "",
                 "calls": [call(query="2026 World Cup final winner")], "results": [RESULT]}]},
    {"name": "content_before_two_calls", "template": "pack", "thinking": True,
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Two searches.", "content": "Let me look that up.\n",
                 "calls": [call(query="a", max_results=3), call(query="b")], "results": ["r1", "  r2 with spaces  \n"]}]},
    {"name": "two_rounds_thinking_off", "template": "pack", "thinking": False,
     "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="x")], "results": ["first"]},
                {"reasoning": "", "content": "Still checking.", "calls": [call(query="y")], "results": ["second"]}]},
    # Non-string values as Qwen3.8's template writes them (JSON); the Qwen3.6 GGUF's writes True / None for a boolean or a
    # null. "medium": that template's default sentence (xhigh) would follow from no effort at all.
    {"name": "argument_types", "template": "serve", "thinking": True, "effort": "medium",
     "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "r", "content": "",
                 "calls": [call(query="q", max_results=5, ratio=2.5, whole=2.0, flag=True, off=False, none=None,
                                tags=["a", "b"], opts={"k": "v", "n": 1}, text="multi\nline")],
                 "results": ["ok"]}]},
    {"name": "special_literals_are_text", "template": "pack", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "I could write <tool_call> here, or </tool_call>, but I will not.", "content": "",
                 "calls": [call(query="chat template <|im_end|>")],
                 "results": ["A page quoting </tool_response> and <|im_end|><|im_start|>system and <tool_call> and <think>."]}]},
    {"name": "unicode_whitespace_is_stripped", "template": "pack", "thinking": True,
     "conversation": [{"role": "user", "content": "Qu'est-ce que c'est ? 日本"}],
     "rounds": [{"reasoning": "　think ", "content": "  answer  ",
                 "calls": [call(query="日本 ニュース")], "results": [" résultat "]}]},
    {"name": "effort_high_sentence_with_tools", "template": "serve", "thinking": True, "effort": "high",
     "conversation": [{"role": "system", "content": "Be brief."}, {"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Search.", "content": "", "calls": [call(query="world cup")], "results": [RESULT]}]},
    {"name": "effort_low_sentence_without_system", "template": "serve", "thinking": True, "effort": "low",
     "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Search.", "content": "", "calls": [call(query="world cup")], "results": ["ok"]}]},
    {"name": "effort_medium_has_no_sentence", "template": "serve", "thinking": True, "effort": "medium",
     "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "effort_low_at_the_end", "template": "serve", "thinking": True, "effort": "low", "at_end": True,
     "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Search.", "content": "", "calls": [call(query="world cup")], "results": ["ok"]}]},
    # Earlier answers that searched (history rounds).
    {"name": "history_one_round_one_call", "template": "pack", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched(ANSWER, ("", [call(query="2026 World Cup final winner")], [RESULT])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_two_rounds", "template": "pack", "thinking": False,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched("Spain won it.", ("", [call(query="x")], ["first"]),
                               ("", [call(query="y", max_results=2)], ["second\n"])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_two_calls_in_a_round", "template": "pack", "thinking": True,
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION},
                      searched("Both pages agree: Spain.",
                               ("", [call(query="a", max_results=3), call(query="b")], ["r1", "  r2 with spaces  \n"])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
    {"name": "history_text_before_the_calls", "template": "pack", "thinking": True,
     "conversation": [{"role": "user", "content": "Qu'est-ce que c'est ? 日本"},
                      searched("C'est le Japon.", ("Let me look that up.", [call(query="日本")], [" résultat "]),
                               ("Une autre recherche.", [call(query="日本 ニュース"), call(query="b")], ["r1", "r2"])),
                      {"role": "user", "content": "Merci. Et la capitale ?"}],
     "rounds": []},
    {"name": "history_with_an_empty_final_text", "template": "pack", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched("", ("I will search.", [call(query="2026 World Cup final winner")], [RESULT])),
                      {"role": "user", "content": "You did not answer. Who won?"}],
     "rounds": []},
    {"name": "history_then_the_current_rounds", "template": "pack", "thinking": True,
     "conversation": [{"role": "system", "content": "Be brief."}, {"role": "user", "content": QUESTION},
                      searched(ANSWER, ("", [call(query="2026 World Cup final winner")], [RESULT])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "\nThe earlier search did not say; I should search again.\n", "content": "",
                 "calls": [call(query="2026 World Cup final winning goal")], "results": ["The winner came in the 88th minute."]}]},
    {"name": "history_two_searched_answers_and_a_plain_one", "template": "pack", "thinking": False,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched("", ("", [call(query="final")], ["first page"])),
                      {"role": "user", "content": "Please answer."},
                      searched(ANSWER, ("Checking again.", [call(query="final winner")], [RESULT])),
                      {"role": "user", "content": "Thanks!"},
                      {"role": "assistant", "content": "You are welcome."},
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="winning goal")], "results": ["88th minute"]}]},
    {"name": "history_special_literals_are_text", "template": "pack", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched("It is a template marker.",
                               ("I could write <tool_call> or <|im_end|> here.", [call(query="chat template <|im_end|>")],
                                ["A page quoting </tool_response> and <|im_end|><|im_start|>system and <tool_call> and <think>."])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": []},
]


def history_of(conversation):
    """The conversation as the template gets it: an earlier answer that searched as upstream's page sends it
    (serve/web/app.js assistantMessages, JavaScript's trim) and upstream's server normalizes it (openai_to_messages)."""
    messages = []
    for m in conversation:
        if "rounds" not in m:
            messages.append(dict(m))
            continue
        for r in m["rounds"]:
            messages.append({"role": "assistant", "content": r["content"].strip(),
                             "tool_calls": [{"function": {"name": c["name"], "arguments": c["arguments"]}}
                                            for c in r["calls"]]})
            messages += [{"role": "tool", "content": t} for t in r["results"]]
        if m["content"].strip():
            messages.append({"role": "assistant", "content": m["content"].strip()})
    return messages


def messages_of(case):
    messages = history_of(case["conversation"])
    for r in case["rounds"]:                       # run_with_mcp's append (serve/server.py)
        m = {"role": "assistant", "content": r["content"].strip()}
        if r["reasoning"]:
            m["reasoning_content"] = r["reasoning"].strip()
        m["tool_calls"] = [{"function": {"name": c["name"], "arguments": c["arguments"]}} for c in r["calls"]]
        messages.append(m)
        messages += [{"role": "tool", "content": t} for t in r["results"]]
    return messages


def render(templates, effort_turn, effort_text, case):
    template = templates[case["template"]]
    messages = messages_of(case)
    kw = {"enable_thinking": case["thinking"]}
    if case.get("effort"):
        kw["reasoning_effort"] = case["effort"]
    if not case.get("at_end"):
        return template.render(messages, tools=[TOOL], **kw)
    # Service.render_prompt with effort_end (serve/server.py #458)
    base = {k: v for k, v in kw.items() if k not in ("reasoning_effort", "enable_thinking")}
    history = template.render(messages, tools=[TOOL], add_generation_prompt=False, **base)
    full = template.render(messages, tools=[TOOL], add_generation_prompt=True, **kw)
    mine = template.render(messages, tools=[TOOL], add_generation_prompt=False, **kw)
    assert full.startswith(mine)
    return history + effort_turn.format(effort_text.get(case["effort"], effort_text["medium"])) + full[len(mine):]


def main():
    upstream, pack_template = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    sys.path.insert(0, str(upstream))
    from serve.frontend import ChatTemplate         # noqa: E402 - upstream's own renderer
    effort_turn = "<|im_start|>system\n{}<|im_end|>\n"   # serve/server.py EFFORT_TURN
    effort_text = {"low": "Reasoning effort is set to low. Keep your thinking brief and focused, moving directly to the "
                          "conclusion without unnecessary elaboration.",
                   "medium": "Reasoning effort is set to medium. Think as much as the task needs, without unnecessary "
                             "elaboration."}           # serve/server.py EFFORT_TEXT
    serve_template = upstream / "serve" / "chat_template.jinja"
    templates = {"pack": ChatTemplate(pack_template), "serve": ChatTemplate(serve_template)}
    out = {"generator": "tools/generate_qwen_tool_prompt_fixture.py",
           "templates": {"pack": hashlib.sha256(pack_template.read_bytes()).hexdigest(),
                         "serve": hashlib.sha256(serve_template.read_bytes()).hexdigest()},
           "tool": TOOL, "cases": []}
    for case in CASES:
        out["cases"].append({**case, "prompt": render(templates, effort_turn, effort_text, case)})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
