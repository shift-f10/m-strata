"""Generate tests/fixtures/glm_tool_prompts.json - the prompts Project Maya renders for a GLM-5.3 chat with tools.

The reference is Maya v1.1.30's own renderer, serve/frontend.py ChatTemplate (Jinja with transformers' settings), over
the GLM-5.3-Flash GGUF's own chat template (tokenizer.chat_template, which Maya's server renders for that pack), imported
from a byte-exact copy of Maya's serve folder (`git -C <Project-maya> show v1.1.30:serve/frontend.py`). The tool rounds
are appended as Maya's run_with_mcp (serve/server.py) appends them: the assistant's content and reasoning (each joined,
then stripped), its calls with their ids, then one `tool` message per result with its tool_call_id.

The template always writes its effort turn, `<|system|>Reasoning Effort: Max` without a level; our GLM profile renders
none (render_glm53_text_prompt, the fixed profile), so each prompt is written without that turn ("effort_turn" records
what was taken out).

An earlier answer that searched (owner 2026-10-29, "keep earlier searches in the chat history as upstream's page does")
is a conversation message with "rounds", as our schema reads the page's strata_rounds. It is expanded as upstream's
page sends it (serve/web/app.js assistantMessages) and its server normalizes it (serve/frontend.py openai_to_messages,
Maya's the same): per round {"role": "assistant", "content": the text before the calls, "tool_calls": [{"function":
{"name", "arguments"}}]} and one {"role": "tool", "content": result} per call (no ids), then {"role": "assistant",
"content": the rest} when the rest is not empty. The rounds are Maya's template's bytes. The text of an earlier answer
is the one other place the fixed profile differs: the template writes its turn without reasoning as
`<|assistant|><think></think>{text}`, the fixed profile as `<|assistant|>{text}`, so each prompt is written without that
think block after the <|assistant|> of an earlier answer's text ("history_think" records what was taken out; a turn
with calls keeps it, as the rounds of this turn do).

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
ANSWER = "Spain won the final 2-1 ([report](https://example.org/final))."
FOLLOW_UP = "Who scored the goals?"


def searched(content, *rounds):
    """An earlier answer that searched, as our schema reads the page's strata_rounds: the rest of its text, its rounds."""
    return {"role": "assistant", "content": content, "rounds": list(rounds)}


def history_round(content, calls, results):
    return {"content": content, "calls": calls, "results": results}


CASES += [
    {"name": "history_one_round_one_call",
     "conversation": [{"role": "user", "content": QUESTION},
                      searched(ANSWER, history_round("", [call(query="2026 World Cup final winner")], [RESULT])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_two_rounds",
     "conversation": [{"role": "user", "content": QUESTION},
                      searched(ANSWER, history_round("", [call(query="x")], ["first"]),
                               history_round("Still checking.", [call(query="y")], ["second"])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_two_calls_in_a_round",
     "conversation": [{"role": "user", "content": QUESTION},
                      searched(ANSWER, history_round("", [call(query="a", max_results=3), call(query="b")],
                                                     ["r1", "  r2 with spaces  \n"])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_text_before_the_calls",
     "conversation": [{"role": "system", "content": "Answer in English."}, {"role": "user", "content": QUESTION},
                      searched(ANSWER, history_round("Let me look that up.", [call(query="2026 World Cup final")], [RESULT])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_answer_without_final_text",
     "conversation": [{"role": "user", "content": QUESTION},
                      searched("", history_round("Spain won it; let me confirm.", [call(query="final")], [RESULT])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_then_this_turns_rounds",
     "conversation": [{"role": "user", "content": QUESTION},
                      searched(ANSWER, history_round("", [call(query="2026 World Cup final winner")], [RESULT])),
                      {"role": "user", "content": FOLLOW_UP}],
     "rounds": [{"reasoning": "I should search for the scorers.", "content": "",
                 "calls": [call(query="2026 World Cup final scorers")], "results": ["Two goals, one each half."]}]},
    {"name": "history_with_a_plain_answer_too",
     "conversation": [{"role": "system", "content": "You are a helpful assistant."}, {"role": "user", "content": "Hi"},
                      {"role": "assistant", "content": "Hello! How can I help?"}, {"role": "user", "content": QUESTION},
                      searched("", history_round("", [call(query="x")], ["first"])),
                      {"role": "user", "content": "And the third place?"},
                      searched("Croatia.", history_round("", [call(query="third place")], ["Croatia beat France."])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
    {"name": "history_text_as_the_template_takes_it",
     "conversation": [{"role": "user", "content": QUESTION},
                      searched(ANSWER, history_round("  I could write <tool_call> or <|user|> here.\n",
                                                     [call(query="glm <|observation|>", max_results=5, flag=True,
                                                           tags=["a", "b"], text="multi\nline")],
                                                     [" A page quoting </tool_response> and <|assistant|>. "])),
                      {"role": "user", "content": FOLLOW_UP}], "rounds": []},
]
HISTORY_THINK = "<think></think>"


def expanded(conversation):
    """The conversation as upstream's page sends it and its server normalizes it (the module's docstring)."""
    messages = []
    for m in conversation:
        if "rounds" not in m:
            messages.append(dict(m))
            continue
        for r in m["rounds"]:
            messages.append({"role": "assistant", "content": r["content"],
                             "tool_calls": [{"function": {"name": c["name"], "arguments": c["arguments"]}} for c in r["calls"]]})
            messages += [{"role": "tool", "content": t} for t in r["results"]]
        if m["content"]:
            messages.append({"role": "assistant", "content": m["content"]})
    return messages


def without_history_think(template, messages, prompt):
    """The prompt as the fixed profile writes an earlier answer's text: the template's <think></think> after that turn's
    <|assistant|> taken out (from the end, so the earlier turns' places hold). Each such turn is found where the
    template renders the messages before it."""
    for i in reversed(range(len(messages))):
        m = messages[i]
        if m["role"] != "assistant" or m.get("tool_calls"):
            continue
        head = template.render(messages[:i], tools=[TOOL], add_generation_prompt=False) + "<|assistant|>" + HISTORY_THINK
        assert prompt.startswith(head), (i, prompt[:len(head)][-80:])
        prompt = prompt[:len(head) - len(HISTORY_THINK)] + prompt[len(head):]
    return prompt


def messages_of(case):
    messages = expanded(case["conversation"])
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
           "history_think": HISTORY_THINK, "tool": TOOL, "cases": []}
    for case in CASES:
        messages = messages_of(case)
        prompt = without_history_think(template, messages, template.render(messages, tools=[TOOL]))
        assert prompt.startswith("[gMASK]<sop>" + EFFORT_TURN), prompt[:80]
        out["cases"].append({**case, "prompt": "[gMASK]<sop>" + prompt[len("[gMASK]<sop>" + EFFORT_TURN):]})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
