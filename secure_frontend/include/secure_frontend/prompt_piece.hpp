#pragma once

#include <string>

namespace secure_frontend {

// A rendered prompt in pieces (web search, docs/superpowers/specs/2026-10-27-web-search-design.md §2), for
// encode_prompt_pieces. A marker is one special token, written by the renderer; text is ordinary text in which a
// special token's literal is refused (the renderer's own text and the client's messages); plain is untrusted text - the
// model's own words and the tools' results - whose literals are ordinary text too, as upstream encodes the literals
// inside a message (serve/frontend.py mark_think_literals, #537/#2026: the span is tokenized with the text around it).
struct PromptPiece final {
  enum class Kind { marker, text, plain };
  Kind kind;
  std::string text;
};

}  // namespace secure_frontend
