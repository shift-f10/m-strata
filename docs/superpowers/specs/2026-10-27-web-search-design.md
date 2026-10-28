# Web search through SearXNG over MCP - design (2026-10-27)

## 0. What the owner asked

Queued 2026-10-27, started the same day ("Commit and proceed to the mcp feature"): "an optional build that is disabled
by default but enabled for us that is an mcp connection to searxng that enables the model to search If it is not
certain and does not have the latest information, that way it does not hallucinate and gives up to date information".

## 1. The references

Upstream Strata (`upstream/serve/mcp.py`, `run_with_mcp` in `serve/server.py`) and Project Maya v1.1.30 (the same
client) already run MCP tools inside a chat (research of 2026-10-27, cited there by file and line):

- the client speaks JSON-RPC 2.0 to stdio servers (newline-delimited) or Streamable HTTP, protocol `2025-06-18`:
  `initialize`, `notifications/initialized`, `tools/list` (paged), `tools/call`, `notifications/cancelled`; a server's
  `ping` is answered, anything else refused (-32601);
- a result's content blocks become text (`text` as is, others a bracketed note), cut at 20,000 characters with a note;
  60 s a call, 120 s to start, at most 8 rounds;
- the loop: a turn that ends (stop) with calls to MCP tools runs them, appends the assistant's tool calls and one
  `tool` message per result, renders the prompt again and generates again; streamed events tell the client what runs;
- each family writes tools in its own template: upstream's Qwen `# Tools ... <tools>{json}</tools>` in the first
  system turn and `<tool_call>\n<function=NAME>\n<parameter=K>\nV\n</parameter>\n</function>\n</tool_call>` replies,
  results back as `<|im_start|>user\n<tool_response>\n...\n</tool_response><|im_end|>`; Maya's GLM `<tool_call>NAME
  <arg_key>K</arg_key><arg_value>V</arg_value></tool_call>` and `<|observation|><tool_response>...</tool_response>`;
- special-token text inside a tool result (a web page quoting `</tool_response>`) is encoded as plain text.

Neither ships a SearXNG server or any text telling the model when to search.

## 2. Our design

- **`strata-search`**, a new program (ours, C++, built only with the option): an MCP server on stdio with one tool,
  `web_search(query, max_results?)`, that asks one SearXNG instance's JSON API (`GET <url>/search?q=...&format=json`)
  and answers a numbered list of title, URL and snippet. It is the only process of the install that connects out for
  search, and only to the configured SearXNG origin (http or https; no redirect followed; a capped body; a timeout).
- **The server's MCP client** (upstream's client, stdio only): it runs exactly that bundled program beside the server
  (never a configured command), in the closed child environment the engines get, with the URL and limits as arguments.
  The server binary's import rule (no resolver, no outbound connect) does not change.
- **When to search**: the tool's description says it - not sure of a fact, the answer depends on recent events or
  anything past the model's training data (with today's date), or the user asks to look something up; answer from the
  results and cite their URLs. No other text is added to a prompt.
- **Tool calls per family**, in each reference's own format: Qwen first (upstream's template and parser), then GLM-5.3
  (Maya's). A card of another profile is served without search. Client-sent `tools` stay refused: the search tool is
  the server's own.
- **The loop** in the chat route, streamed or not: up to `search.max_rounds` rounds (upstream's 8); a call runs only
  when the turn ended as stop and names `web_search`; the answer streams across rounds, with a `strata_search` event
  for each search (its query) and each result (found or failed). Usage: the rounds' completion tokens summed, the last
  round's prompt.
- **Config** (refused by name in a build without the option, as `alerts.*` are): `search.enabled`, `search.url`,
  `search.results` (1-10, 5), `search.max_rounds` (1-8, 8), `search.timeout_s` (5-120, 30). The page's Settings tab gets
  a Web search section in a build that has it.
- **Build**: `SECURE_FRONTEND_WEB_SEARCH` (off by default; on in our presets and `make --with search`).

## 3. Threats this adds (for the threat model)

- Search queries leave the machine through SearXNG to the search engines it asks: what the model searches for is
  derived from the conversation.
- Results are untrusted text in the prompt: a page can try to steer the model (prompt injection). Only titles, URLs and
  snippets reach it (no page bodies), never as special tokens; the model has no other tool, so a steered model can only
  search again or answer.
- `strata-search` is a network client: one origin, GET only, no redirects, bounded size and time, TLS checked as the
  email client checks it.

## 4. Phases (TDD; RED recorded for each)

1. `strata-search`: the MCP server and the SearXNG client, against a fake SearXNG on loopback.
2. The server's MCP client, against `strata-search` and a fake MCP server.
3. Qwen tool calls: the renderer (tools, calls, results), the encoder (results as plain text), the reply's tool
   channel and upstream's parser.
4. The loop in the chat route (non-streamed, then streamed), the config rows and the build option.
5. GLM-5.3's format.
6. The page's Settings section; make, egress tests, docs; an end-to-end run against a real SearXNG.

## 5. Decisions taken while building (2026-10-27)

- The prompts are checked byte for byte against the references' own renderers, run on fixtures: upstream's
  `ChatTemplate` over the Qwen3.6 GGUF's template (and Qwen3.8's, `serve/chat_template.jinja`), Maya's over GLM-5.3-Flash's;
  the parsers against upstream's and Maya's `OutputParser`, fed whole and character by character.
- Qwen: a non-string argument is written back as JSON (Qwen3.8's template); the Qwen3.6 GGUF's writes `True`/`None` for a
  boolean or a null, which `web_search` never takes. Earlier assistant turns keep the fixed profile's form.
- GLM-5.3: the fixed profile has no `Reasoning Effort` system turn (the template writes "Max" by default); the tool prompt
  keeps the fixed profile's form there. Maya's parser reads any tool name and fails a request on a malformed call; ours
  reads the declared tool only and leaves anything else as the text it was (our clients declare no tools).
- A request with images is served without search (the image embeddings belong to one generation).
- The server starts `strata-search` on the first chat that needs it and keeps it; a failed start leaves chats without
  search for 60 s rather than making each wait for it.
- The page's settings apply at the next start, as the alert settings do; a test search runs through the saved settings.

## 6. After the owner's tests (2026-10-28)

The owner's Qwen3.6 searched eight times and gave no answer; raon1 and flint did not search at all. What changed:

- **An answer round after the limit.** Upstream ends the turn when the rounds run out (`serve/server.py` `run_with_mcp`),
  so a model still searching gives no answer. Ours: past `search.max_rounds`, the round's calls are not run; each one's
  result is a fixed note ("Not run: this answer has made all the searches it may. Answer the user now from the results
  above, without searching again."), the `limit` event and a `skipped` result go out, and the model gets one more round;
  a call in that round ends the answer as before. The page notes the limit in the search box.
- **Why a search came back empty.** SearXNG lists the engines that did not answer (`unresponsive_engines`); an empty
  result now says so ("No results: the search engines did not answer (brave: ...; duckduckgo: CAPTCHA), so searching
  again now is unlikely to help."), at most 8 of them, each name and reason cut short.
- **raon1 and flint.** raon-1-mini's template is Qwen's (`<tools>`, `<tool_call>` with `<function=...>`, results as
  `<tool_response>` in a user turn) and its parser is Qwen's; flint's is GLM's (`<arg_key>`/`<arg_value>`, results after
  `<|observation|>`) and its parser is GLM's. Both vocabularies have the tags as single tokens. The renderers are checked
  byte for byte against upstream's `ChatTemplate` over each model's own `chat_template.jinja` (raon1's needs the
  `{% generation %}` extension), 19 fixture cases (`tools/generate_raon1_flint_tool_prompt_fixture.py`). gemma4 and
  deepseek are still served without search.
- **Turning it off.** A card's **Web search** checkbox (the `web_search` option of every qwen, glm53, raon1 and flint
  card, on by default, listed only while the server's search is on; it writes no flag and no row) decides whether a
  started model gets the tool; calibration leaves it out of its key. A request's `"strata_search": false` (any profile;
  anything but a boolean is a 400 naming it) serves that chat without the tool; the page sends it when the chat
  settings' Web search switch is off, a switch shown only while the running card has Web search on.
