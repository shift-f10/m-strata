"""Generate tests/fixtures/glm_tool_prompts.json - the prompts Project Maya renders for a GLM-5.3 chat with tools.

The reference is Maya v1.1.30's own renderer, serve/frontend.py ChatTemplate (Jinja with transformers' settings), over
the GLM-5.3-Flash GGUF's own chat template (tokenizer.chat_template, which Maya's server renders for that pack), imported
from a byte-exact copy of Maya's serve folder (`git -C <Project-maya> show v1.1.30:serve/frontend.py`). The tool rounds
are appended as Maya's run_with_mcp (serve/server.py) appends them: the assistant's content and reasoning (each joined,
then stripped), its calls with their ids, then one `tool` message per result with its tool_call_id.

The template always writes its effort turn, `<|system|>Reasoning Effort: Max` without a level; our GLM profile renders
none (render_glm53_text_prompt, the fixed profile), so each prompt is written without that turn ("effort_turn" records
what was taken out). The cases hold no earlier assistant turn, the one other place the fixed profile differs.

Usage: python -I tools/generate_glm_tool_prompt_fixture.py <Maya serve dir's parent> <GLM-5.3 chat template>
           > tests/fixtures/glm_tool_prompts.json
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
EFFORT_TURN = "<|system|>Reasoning Effort: Max"


def call(**arguments):
    return {"name": "web_search", "arguments": arguments}


CASES = [
    {"name": "first_prompt_with_tools", "conversation": [{"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "system_and_tools", "conversation": [{"role": "system", "content": "You are a helpful assistant."},
                                                  {"role": "user", "content": QUESTION}], "rounds": []},
    {"name": "one_round_a_call_after_reasoning", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "\nThe user asks about a recent event; I should search.\n", "content": "",
                 "calls": [call(query="2026 World Cup final winner")], "results": [RESULT]}]},
    {"name": "content_before_two_calls",
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "Two searches.", "content": "Let me look that up.\n",
                 "calls": [call(query="a", max_results=3), call(query="b")], "results": ["r1", "  r2 with spaces  \n"]}]},
    {"name": "two_rounds", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "", "content": "", "calls": [call(query="x")], "results": ["first"]},
                {"reasoning": "More.", "content": "Still checking.", "calls": [call(query="y")], "results": ["second"]}]},
    {"name": "argument_types", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "r", "content": "",
                 "calls": [call(query="q", max_results=5, ratio=2.5, whole=2.0, flag=True, none=None, tags=["a", "b"],
                                opts={"k": "v", "n": 1}, text="multi\nline")],
                 "results": ["ok"]}]},
    {"name": "special_literals_are_text", "conversation": [{"role": "user", "content": QUESTION}],
     "rounds": [{"reasoning": "I could write <tool_call> or <arg_key> here.", "content": "",
                 "calls": [call(query="glm <|observation|>")],
                 "results": ["A page quoting </tool_response> and <|user|> and <arg_value>."]}]},
    {"name": "unicode_whitespace_is_stripped", "conversation": [{"role": "user", "content": "Qu'est-ce que c'est ? 日本"}],
     "rounds": [{"reasoning": "　think ", "content": "  answer  ",
                 "calls": [call(query="日本 ニュース")], "results": [" résultat "]}]},
]


def messages_of(case):
    messages = [dict(m) for m in case["conversation"]]
    n = 0
    for r in case["rounds"]:                       # Maya's run_with_mcp (serve/server.py)
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


def main():
    maya, template_path = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    sys.path.insert(0, str(maya))
    from serve.frontend import ChatTemplate         # noqa: E402 - Maya's own renderer
    template = ChatTemplate(template_path)
    out = {"generator": "tools/generate_glm_tool_prompt_fixture.py",
           "template": hashlib.sha256(template_path.read_bytes()).hexdigest(), "effort_turn": EFFORT_TURN,
           "tool": TOOL, "cases": []}
    for case in CASES:
        prompt = template.render(messages_of(case), tools=[TOOL])
        assert prompt.startswith("[gMASK]<sop>" + EFFORT_TURN), prompt[:80]
        out["cases"].append({**case, "prompt": "[gMASK]<sop>" + prompt[len("[gMASK]<sop>" + EFFORT_TURN):]})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
