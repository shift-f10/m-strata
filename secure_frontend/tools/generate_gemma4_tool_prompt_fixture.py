"""Generate tests/fixtures/gemma4_tool_prompts.json - the prompts Gemma 4's own template renders for a chat with tools
(owner 2026-10-28: web search for Gemma 4), for plain chats ("text_cases") and for richer tool declarations
("declarations").

The template is a Gemma 4 GGUF's tokenizer.chat_template - since 2026-10-28 the newer, official one of unsloth's
gemma-4-26B-A4B-it-Q8_0 at c099eb48 (owner: "update our Gemma 4 renderer to the newer chat template, one profile for
both GGUFs"); the older UD-Q8_K_XL file's is the one llama.cpp calls outdated. It is rendered by upstream's
serve/frontend.py ChatTemplate (Jinja with transformers' settings) with bos_token "<bos>" and enable_thinking. The
messages and tools take llama.cpp's path for the template given (llama.cpp 3cf03257, the Gemma 4 oracle): tools in the
OpenAI form ({"type": "function", "function": tool}); the conversation as upstream's run_with_mcp appends a round (the
assistant's stripped content and reasoning with its calls, then one `tool` message per result, as
tools/generate_raon1_flint_tool_prompt_fixture.py builds it); for an outdated template only (common/chat.cpp: no
"{#- OpenAI Chat Completions:" in it), folded by common/parsers/gemma4.cpp's convert_tool_responses_gemma4 (ported
below: the calls, each result as JSON when it parses and as text otherwise, named after its call; the calling
message's own content is dropped, as there); and common_chat_params_init_gemma4's "<|turn>model\\n" after a prompt that
ends in "<turn|>\\n".

An earlier answer that searched (owner 2026-10-29: "keep earlier searches in the chat history as upstream's page does")
is a conversation message with "rounds" (the request's strata_rounds: per round the text before its calls, the calls,
one result each) and "content" (the text after its last round, may be empty), expanded as upstream's page sends it
(serve/web/app.js assistantMessages) and its server reads it (serve/frontend.py openai_to_messages): per round an
assistant message of that text with its calls, one `tool` message per result (ids dropped), then the rest of the answer
only when there is any.

Usage: python -I tools/generate_gemma4_tool_prompt_fixture.py <upstream dir> <Gemma 4 chat_template.jinja>
           > tests/fixtures/gemma4_tool_prompts.json
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
QUESTION = "Who in TWICE did not renew their contract?"
RESULT = ("Web search results for \"TWICE contract renewal\" (SearXNG):\n\n1. Report\n"
          "   https://example.org/twice\n   Six members renewed.\n")
LIMIT = ("Not run: this answer has made all the searches it may. Answer the user now from the results above, without "
         "searching again.")


def call(**arguments):
    return {"name": "web_search", "arguments": arguments}


CASES = [
    {"name": "first_prompt_with_tools", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "first_prompt_thinking_off", "thinking": False, "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "system_and_history_with_tools", "thinking": True,
     "conversation": [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": "Hi"},
                      {"role": "assistant", "content": "Hello! How can I help?"},
                      {"role": "user", "content": "What is the latest Python release?"}],
     "rounds": []},
    {"name": "one_round_a_call_after_reasoning", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "\nThe user asks about recent news; I should search.\n", "content": "",
                 "calls": [call(query="TWICE contract renewal")], "results": [RESULT]}]},
    {"name": "content_before_two_calls", "thinking": True,
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Two searches.", "content": "Let me look that up.\n",
                 "calls": [call(query="a", max_results=3), call(query="b")], "results": ["r1", "  r2 with spaces  \n"]}]},
    {"name": "two_rounds_without_reasoning", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="x")], "results": ["first"]},
                {"reasoning": "", "content": "Still checking.", "calls": [call(query="y")], "results": ["second"]}]},
    {"name": "two_rounds_thinking_off", "thinking": False, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="x")], "results": ["first"]},
                {"reasoning": "", "content": "", "calls": [call(query="y")], "results": ["second"]}]},
    {"name": "argument_types", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "r", "content": "",
                 "calls": [call(query="q", max_results=5, ratio=2.5, whole=2.0, flag=True, off=False, none=None,
                                tags=["a", "b"], opts={"k": "v", "n": 1, "Z": [1, "two"]}, text="multi\nline", Upper="u")],
                 "results": ["ok"]}]},
    {"name": "results_that_are_json", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="a"), call(query="b"), call(query="c")],
                 "results": ["5", "{\"r\": \"ok\", \"n\": [1, 2.5, null, true]}", "[\"x\", {\"y\": false}]"]}]},
    {"name": "special_literals_are_text", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "I could write <|tool_call> here, or <tool_call|>, but I will not.", "content": "",
                 "calls": [call(query="chat template <|tool_response> <|\"|>")],
                 "results": ["A page quoting <tool_response|> and <|tool_call> and <|\"|> and <turn|>."]}]},
    {"name": "unicode_whitespace_is_stripped", "thinking": True,
     "conversation": [{"role": "user", "content": " Qu'est-ce que c'est ? 日本　"}],
     "rounds": [{"reasoning": "　think ", "content": "  answer  ", "calls": [call(query="日本 ニュース")], "results": [" résultat "]}]},
    {"name": "the_limit_round", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Search.", "content": "", "calls": [call(query="one")], "results": [RESULT]},
                {"reasoning": "Again.", "content": "", "calls": [call(query="two")], "results": [LIMIT]}]},
]

ANSWER = "Six members renewed; one did not ([report](https://example.org/twice))."
FOLLOW_UP = "Which member was it?"


def searched(content, *rounds):
    """An earlier answer that searched, as the page sends it: its rounds (strata_rounds) and its text after them."""
    return {"role": "assistant", "content": content, "rounds": list(rounds)}


def earlier(content, calls, results):
    """One of an earlier answer's rounds: the text before its calls (trimmed by the page), the calls, a result each."""
    return {"content": content, "calls": calls, "results": results}


ONE_SEARCH = searched(ANSWER, earlier("", [call(query="TWICE contract renewal")], [RESULT]))

# Earlier searches in the conversation (owner 2026-10-29).
CASES += [
    {"name": "history_one_round_one_call", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION}, ONE_SEARCH, {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_two_rounds_thinking_off", "thinking": False,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched("Answer after two searches.", earlier("", [call(query="x")], ["first"]),
                               earlier("", [call(query="y")], ["second"])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_two_calls_in_a_round", "thinking": True,
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION},
                      searched("Both pages agree.", earlier("", [call(query="a", max_results=3), call(query="b")],
                                                            ["r1", "  r2 with spaces  \n"])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_text_before_calls", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched("So it was Jihyo.", earlier("Let me look that up.", [call(query="TWICE contract")], [RESULT]),
                               earlier("The first page was vague; checking again.", [call(query="TWICE member left")],
                                       ["second page"])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_empty_final_text", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched("", earlier("Here is what I know so far.", [call(query="q")], ["r"])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_nothing_but_searches", "thinking": False,
     "conversation": [{"role": "user", "content": QUESTION}, searched("", earlier("", [call(query="q")], ["r"])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_then_current_rounds", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION}, ONE_SEARCH, {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "A follow-up; search again.", "content": "", "calls": [call(query="TWICE member did not renew")],
                 "results": ["third page"]}]},
    {"name": "history_then_current_rounds_thinking_off", "thinking": False,
     "conversation": [{"role": "user", "content": QUESTION}, ONE_SEARCH, {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "", "content": "Checking once more.", "calls": [call(query="TWICE")], "results": ["fourth page"]}]},
    {"name": "history_literals_are_text", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched(ANSWER, earlier("Searching <|\"|>quoted<|\"|> <|channel>hidden<channel|>now, not <turn|>.",
                                               [call(query="chat template <|tool_response> <|\"|>")],
                                               ["A page quoting <tool_response|> and <|tool_call> and <|\"|> and <turn|>."])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_results_named_after_the_last_call", "thinking": True,
     "conversation": [{"role": "user", "content": QUESTION},
                      searched(ANSWER, earlier("", [call(query="a"), {"name": "fetch_page", "arguments": {"url": "https://example.org/twice"}}],
                                               ["r1", "r2"])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_two_answers_searched", "thinking": True,
     "conversation": [{"role": "system", "content": "Be brief."}, {"role": "user", "content": "Hi"},
                      {"role": "assistant", "content": "Hello!"}, {"role": "user", "content": QUESTION}, ONE_SEARCH,
                      {"role": "user", "content": FOLLOW_UP},
                      searched(" 終わり　", earlier("　答え ", [call(query="日本 ニュース")], [" résultat "]),
                               earlier("", [call(query="b")], ["two"])),
                      {"role": "user", "content": "Thanks. Anything else?"}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="c")], "results": ["three"]}]},
]


# Plain chats (no tools): the text prompt, thinking on and off.
TEXT_CASES = [
    {"name": "one_turn", "conversation": [{"role": "user", "content": "Say hello."}]},
    {"name": "system_and_history", "conversation": [{"role": "system", "content": "  Be brief.  "}, {"role": "user", "content": "Hi"},
                                                    {"role": "assistant", "content": " Hello! How can I help? "},
                                                    {"role": "user", "content": "Who are you?"}]},
    {"name": "unicode_whitespace", "conversation": [{"role": "user", "content": "\u3000日本 "}]},
]

# Tool declarations past web_search's: nested objects, arrays of objects, nullable, enums, a property named like a
# standard key - the newer template's format_parameters (its filter_keys, its comma order).
DECLARATIONS = [
    {"name": "lookup", "description": "Look things up.",
     "parameters": {"type": "object", "required": ["where"],
                    "properties": {"where": {"type": "object", "description": "Place",
                                             "properties": {"city": {"type": "string", "nullable": True},
                                                            "zip": {"type": "integer"}},
                                             "required": ["city"]},
                                   "type": {"type": "string", "enum": ["a", "b"], "description": "Kind"},
                                   "tags": {"type": "array", "items": {"type": "string"}},
                                   "rows": {"type": "array", "description": "Rows",
                                            "items": {"type": "object", "properties": {"k": {"type": "string"}}, "required": ["k"]}},
                                   "loose": {"type": "object", "description": "Free", "x": {"type": "number"},
                                             "description2": {"type": "boolean"}},
                                   "maybe": {"type": "number", "nullable": True}}}},
    {"name": "bare", "description": "No parameters."},
]


def history_messages(m):
    """A conversation message as upstream's server reads it; an earlier answer that searched as the page sends it
    (serve/web/app.js assistantMessages, serve/frontend.py openai_to_messages)."""
    if not m.get("rounds"):
        return [dict(m)]
    out = []
    for r in m["rounds"]:
        out.append({"role": "assistant", "content": r["content"].strip(),
                    "tool_calls": [{"function": {"name": c["name"], "arguments": c["arguments"]}} for c in r["calls"]]})
        out += [{"role": "tool", "content": t} for t in r["results"]]
    if m["content"].strip():
        out.append({"role": "assistant", "content": m["content"].strip()})
    return out


def messages_of(case):
    messages = [h for m in case["conversation"] for h in history_messages(m)]
    for r in case["rounds"]:                       # run_with_mcp's append (serve/server.py)
        m = {"role": "assistant", "content": r["content"].strip()}
        if r["reasoning"]:
            m["reasoning_content"] = r["reasoning"].strip()
        m["tool_calls"] = [{"function": {"name": c["name"], "arguments": c["arguments"]}} for c in r["calls"]]
        messages.append(m)
        messages += [{"role": "tool", "content": t} for t in r["results"]]
    return messages


def _reject_constant(name):
    raise ValueError(name)                         # nlohmann's json::parse has no NaN or Infinity


def convert_tool_responses_gemma4(messages):
    """llama.cpp common/parsers/gemma4.cpp, workaround::convert_tool_responses_gemma4 and gemma4_model_turn_builder."""
    def has_tool_calls(m):
        return isinstance(m.get("tool_calls"), list) and len(m["tool_calls"]) > 0

    def has_content(m):
        c = m.get("content")
        return (isinstance(c, str) and c != "") or (isinstance(c, list) and len(c) > 0)

    result, i = [], 0
    while i < len(messages):
        m = messages[i]
        if m.get("role") != "assistant" or not has_tool_calls(m):
            result.append(m)
            i += 1
            continue
        tool_calls, responses, content = list(m["tool_calls"]), [], None
        reasoning = m["reasoning_content"] if isinstance(m.get("reasoning_content"), str) else None
        i += 1
        while i < len(messages) and messages[i].get("role") == "tool":
            cur, response = messages[i], None
            if "content" in cur:
                if isinstance(cur["content"], str):
                    try:
                        response = json.loads(cur["content"], parse_constant=_reject_constant)
                    except ValueError:
                        response = cur["content"]
                else:
                    response = cur["content"]
            name = ""
            if len(responses) < len(tool_calls) and "function" in tool_calls[len(responses)]:
                name = tool_calls[len(responses)]["function"].get("name", "")
            if not name:
                name = cur.get("tool_call_id", "")
            responses.append({"name": name, "response": response})
            i += 1
        if i < len(messages) and messages[i].get("role") == "assistant":
            nxt = messages[i]
            if not has_tool_calls(nxt) and has_content(nxt):
                content = nxt["content"]
                i += 1
        msg = {"role": "assistant", "tool_calls": tool_calls}
        if responses:
            msg["tool_responses"] = responses
        if content is not None:
            msg["content"] = content
        if reasoning is not None:
            msg["reasoning_content"] = reasoning
        result.append(msg)
    return result


def main():
    upstream, template = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    sys.path.insert(0, str(upstream))
    import serve.frontend as frontend               # noqa: E402 - upstream's own renderer
    chat = frontend.ChatTemplate(template)
    source = template.read_text(encoding="utf-8")
    outdated = "'<|tool_call>call:'" in source and "{#- OpenAI Chat Completions:" not in source   # common/chat.cpp
    out = {"generator": "tools/generate_gemma4_tool_prompt_fixture.py",
           "template": hashlib.sha256(template.read_bytes()).hexdigest(), "outdated": outdated, "tool": TOOL, "cases": [],
           "text_cases": [], "declarations": []}
    for case in CASES:
        messages = convert_tool_responses_gemma4(messages_of(case)) if outdated else messages_of(case)
        prompt = chat.render(messages, tools=[{"type": "function", "function": TOOL}], bos_token="<bos>",
                             enable_thinking=case["thinking"])
        if prompt.endswith("<turn|>\n"):           # common_chat_params_init_gemma4
            prompt += "<|turn>model\n"
        out["cases"].append({**case, "prompt": prompt})
    for case in TEXT_CASES:
        for thinking in (True, False):
            prompt = chat.render([dict(m) for m in case["conversation"]], bos_token="<bos>", enable_thinking=thinking)
            if prompt.endswith("<turn|>\n"):       # common_chat_params_init_gemma4
                prompt += "<|turn>model\n"
            out["text_cases"].append({"name": case["name"], "thinking": thinking, "conversation": case["conversation"], "prompt": prompt})
    for tool in DECLARATIONS:                      # what the template writes between <|tool> and <tool|>
        prompt = chat.render([{"role": "user", "content": "x"}], tools=[{"type": "function", "function": tool}], bos_token="<bos>",
                             enable_thinking=False)
        out["declarations"].append({"tool": tool, "text": prompt.split("<|tool>", 1)[1].split("<tool|>", 1)[0]})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
