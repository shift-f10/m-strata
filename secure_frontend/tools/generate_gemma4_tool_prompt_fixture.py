"""Generate tests/fixtures/gemma4_tool_prompts.json - the prompts Gemma 4's own template renders for a chat with tools
(owner 2026-10-28: web search for Gemma 4).

The template is the UD-Q8_K_XL GGUF's tokenizer.chat_template, rendered by upstream's serve/frontend.py ChatTemplate
(Jinja with transformers' settings) with bos_token "<bos>" and enable_thinking. The messages and tools take llama.cpp's
path for this template (llama.cpp 3cf03257, the Gemma 4 oracle): tools in the OpenAI form ({"type": "function",
"function": tool}); the conversation as upstream's run_with_mcp appends a round (the assistant's stripped content and
reasoning with its calls, then one `tool` message per result, as tools/generate_raon1_flint_tool_prompt_fixture.py
builds it), folded by common/parsers/gemma4.cpp's convert_tool_responses_gemma4 (ported below: the calls, each result
as JSON when it parses and as text otherwise, named after its call; the calling message's own content is dropped, as
there), since this GGUF's template is the one llama.cpp calls outdated; and common_chat_params_init_gemma4's
"<|turn>model\\n" after a prompt that ends in "<turn|>\\n".

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
    out = {"generator": "tools/generate_gemma4_tool_prompt_fixture.py",
           "template": hashlib.sha256(template.read_bytes()).hexdigest(), "tool": TOOL, "cases": []}
    for case in CASES:
        messages = convert_tool_responses_gemma4(messages_of(case))
        prompt = chat.render(messages, tools=[{"type": "function", "function": TOOL}], bos_token="<bos>",
                             enable_thinking=case["thinking"])
        if prompt.endswith("<turn|>\n"):           # common_chat_params_init_gemma4
            prompt += "<|turn>model\n"
        out["cases"].append({**case, "prompt": prompt})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
