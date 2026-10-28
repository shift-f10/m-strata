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
]


def messages_of(case):
    messages = [dict(m) for m in case["conversation"]]
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
