"""Generate tests/fixtures/raon1_flint_tool_prompts.json - the prompts raon-1-mini's and flint's own templates render for a
chat with tools (owner 2026-10-28: web search for raon1 and flint).

The reference is upstream's own renderer, serve/frontend.py ChatTemplate (Jinja with transformers' settings), over each
model's tokenizer/chat_template.jinja: raon-1-mini's (KRAFTON's, with enable_thinking) and flint's (the GGUF's; it has no
thinking switch). The tool rounds are appended as upstream's run_with_mcp (serve/server.py) appends them: the
assistant's content and reasoning (each stripped), its tool calls, then one `tool` message per result - the same as
tools/generate_qwen_tool_prompt_fixture.py.

Usage: python -I tools/generate_raon1_flint_tool_prompt_fixture.py <upstream dir> <raon1 chat_template.jinja>
           <flint chat_template.jinja> > tests/fixtures/raon1_flint_tool_prompts.json
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


def cases_for(template):
    thinking = template == "raon1"
    out = [
        {"name": "first_prompt_with_tools", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
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
        {"name": "argument_types", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
         "rounds": [{"reasoning": "r", "content": "",
                     "calls": [call(query="q", max_results=5, ratio=2.5, whole=2.0, flag=True, off=False, none=None,
                                    tags=["a", "b"], opts={"k": "v", "n": 1}, text="multi\nline")],
                     "results": ["ok"]}]},
        {"name": "special_literals_are_text", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
         "rounds": [{"reasoning": "I could write <tool_call> here, or </tool_call>, but I will not.", "content": "",
                     "calls": [call(query="chat template <tool_response>")],
                     "results": ["A page quoting </tool_response> and <tool_call> and <arg_key>."]}]},
        {"name": "unicode_whitespace_is_stripped", "thinking": True,
         "conversation": [{"role": "user", "content": "Qu'est-ce que c'est ? 日本"}],
         "rounds": [{"reasoning": "　think ", "content": "  answer  ", "calls": [call(query="日本 ニュース")], "results": [" résultat "]}]},
        {"name": "the_limit_round", "thinking": True, "conversation": [{"role": "user", "content": QUESTION}],
         "rounds": [{"reasoning": "Search.", "content": "", "calls": [call(query="one")], "results": [RESULT]},
                    {"reasoning": "Again.", "content": "", "calls": [call(query="two")], "results": [LIMIT]}]},
    ]
    if thinking:
        out.append({"name": "two_rounds_thinking_off", "thinking": False, "conversation": [{"role": "user", "content": QUESTION}],
                    "rounds": [{"reasoning": "", "content": "", "calls": [call(query="x")], "results": ["first"]}]})
    for case in out:
        case["template"] = template
    return out


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


def with_generation_tag(frontend):
    """raon-1-mini's template marks the assistant's body with {% generation %} ... {% endgeneration %}, transformers'
    extension for its assistant-token mask; transformers renders the body as it is. Upstream's ChatTemplate does not
    load that extension, so its environment gets it here - nothing else about upstream's settings changes."""
    from jinja2 import nodes
    from jinja2.ext import Extension

    class Generation(Extension):
        tags = {"generation"}

        def parse(self, parser):
            lineno = next(parser.stream).lineno
            body = parser.parse_statements(("name:endgeneration",), drop_needle=True)
            return nodes.CallBlock(self.call_method("_body", []), [], [], body).set_lineno(lineno)

        def _body(self, caller):
            return caller()

    base = frontend.ImmutableSandboxedEnvironment

    class Environment(base):
        def __init__(self, *args, **kwargs):
            kwargs["extensions"] = list(kwargs.get("extensions", [])) + [Generation]
            super().__init__(*args, **kwargs)

    frontend.ImmutableSandboxedEnvironment = Environment


def main():
    upstream, raon1, flint = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
    sys.path.insert(0, str(upstream))
    import serve.frontend as frontend               # noqa: E402 - upstream's own renderer
    with_generation_tag(frontend)
    ChatTemplate = frontend.ChatTemplate
    templates = {"raon1": ChatTemplate(raon1), "flint": ChatTemplate(flint)}
    out = {"generator": "tools/generate_raon1_flint_tool_prompt_fixture.py",
           "templates": {"raon1": hashlib.sha256(raon1.read_bytes()).hexdigest(),
                         "flint": hashlib.sha256(flint.read_bytes()).hexdigest()},
           "tool": TOOL, "cases": []}
    for name in ("raon1", "flint"):
        for case in cases_for(name):
            kw = {"enable_thinking": case["thinking"]} if name == "raon1" else {}
            out["cases"].append({**case, "prompt": templates[name].render(messages_of(case), tools=[TOOL], **kw)})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
