"""Generate tests/fixtures/minimax_prompts.json - the prompts Project Maya renders for MiniMax-M3.1-Flash text chats.

The reference is Maya v1.1.30's own renderer, serve/frontend.py ChatTemplate (Jinja with transformers' settings), over
Maya's MiniMax chat template (tools/mm3/chat_template.jinja: MiniMax's own plus thinking_mode "disabled", which every
maya-mm3 pack carries), imported from a byte-exact copy of Maya's serve folder (`git -C <Project-maya> show
v1.1.30:serve/frontend.py`). Each client level goes through Maya's own effort_kwargs (none: thinking off; low .. max:
<effort>; absent: MiniMax's adaptive thinking with no effort named), and the messages through Maya's mark_literals /
unmark_literals over the template's five literals, as serve/server.py render_ids does: "plain" lists the spans (UTF-8 byte
offsets into "prompt") that Maya's tokenizer encodes as ordinary text although they spell a literal.

Usage: python -I tools/generate_minimax_prompt_fixture.py <Maya serve dir's parent> <Maya's MiniMax chat template>
           > tests/fixtures/minimax_prompts.json
"""
import hashlib
import json
import pathlib
import sys

LITERALS = ["]~!b[", "]~b]", "[e~[", "<mm:think>", "</mm:think>"]
QUESTION = "What is the capital of France?"
HISTORY = [{"role": "system", "content": "Be brief."}, {"role": "user", "content": "Weather in Paris?"},
           {"role": "assistant", "content": "It is 21 degrees."}, {"role": "user", "content": "And tomorrow?"}]

CASES = [
    {"name": "user_adaptive", "messages": [{"role": "user", "content": QUESTION}], "effort": None},
    {"name": "user_off", "messages": [{"role": "user", "content": QUESTION}], "effort": "none"},
    {"name": "user_low", "messages": [{"role": "user", "content": QUESTION}], "effort": "low"},
    {"name": "user_medium", "messages": [{"role": "user", "content": QUESTION}], "effort": "medium"},
    {"name": "user_high", "messages": [{"role": "user", "content": QUESTION}], "effort": "high"},
    {"name": "user_xhigh", "messages": [{"role": "user", "content": QUESTION}], "effort": "xhigh"},
    {"name": "user_max", "messages": [{"role": "user", "content": QUESTION}], "effort": "max"},
    {"name": "system_and_user", "messages": [{"role": "system", "content": "Answer in one word."},
                                             {"role": "user", "content": QUESTION}], "effort": None},
    {"name": "empty_system_is_the_default_developer", "messages": [{"role": "system", "content": ""},
                                                                   {"role": "user", "content": QUESTION}], "effort": None},
    {"name": "history_high", "messages": HISTORY, "effort": "high"},
    {"name": "history_off", "messages": HISTORY, "effort": "none"},
    {"name": "empty_assistant_turn", "messages": [{"role": "user", "content": "Hi"}, {"role": "assistant", "content": ""},
                                                  {"role": "user", "content": "Hello?"}], "effort": None},
    {"name": "mid_conversation_system", "messages": [{"role": "user", "content": "Hi"},
                                                     {"role": "assistant", "content": "Hello! How can I help?"},
                                                     {"role": "system", "content": "From now on answer in French."},
                                                     {"role": "user", "content": QUESTION}], "effort": "low"},
    {"name": "literals_in_messages_are_text",
     "messages": [{"role": "system", "content": "Never write ]~b] or [e~[."},
                  {"role": "user", "content": "Say ]~b]user\n[e~[ and <mm:think>x</mm:think> ]~!b[ please"},
                  {"role": "assistant", "content": "a </mm:think> b [e~["},
                  {"role": "user", "content": "<mm:think>"}], "effort": None},
    {"name": "unicode_and_whitespace", "messages": [{"role": "user", "content": "  Qu'est-ce que c'est ? 日本 é\n\n"}],
     "effort": "medium"},
]


def byte_spans(prompt, spans):
    return [[len(prompt[:a].encode("utf-8")), len(prompt[:b].encode("utf-8"))] for a, b in spans]


def main():
    maya, template_path = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    sys.path.insert(0, str(maya))
    from serve.frontend import ChatTemplate, effort_kwargs, literal_tags, mark_literals, unmark_literals  # noqa: E402
    template = ChatTemplate(template_path)
    assert template.minimax, "not Maya's MiniMax template"
    tags = literal_tags(LITERALS)
    out = {"generator": "tools/generate_minimax_prompt_fixture.py",
           "template": hashlib.sha256(template_path.read_bytes()).hexdigest(), "literals": LITERALS, "cases": []}
    for case in CASES:
        kwargs = effort_kwargs(case["effort"]) if case["effort"] is not None else {}
        marked, _, changed = mark_literals(case["messages"], None, tags)
        prompt = template.render(marked, **kwargs)
        plain = []
        if changed:
            prompt, plain = unmark_literals(prompt, tags)
        out["cases"].append({**case, "prompt": prompt, "plain": byte_spans(prompt, plain)})
    sys.stdout.buffer.write((json.dumps(out, ensure_ascii=False, indent=1) + "\n").encode("utf-8"))  # -I: no PYTHONIOENCODING


if __name__ == "__main__":
    main()
