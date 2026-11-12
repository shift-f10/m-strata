#include "secure_frontend/jinja_renderer.hpp"
#include "support/test_support.hpp"
#include <iostream>

namespace {
int failures = 0;
void expect(bool ok, const char* message) { if (!ok) { ++failures; std::cerr << message << '\n'; } }
using namespace secure_frontend;
const char* context = R"({"messages":[{"role":"user","content":"<|evil|> hello"}],"tools":[{"type":"function","function":{"name":"lookup","description":"<|evil|>"}}],"add_generation_prompt":true,"enable_thinking":false,"bos_token":"<s>"})";
void native_templates_render_messages_tools_and_mark_input() {
  const auto result = render_jinja_template(jinja_helper_path(),
      "{{ bos_token }}{% for m in messages %}<|{{ m.role }}|>{{ m.content }}{% endfor %}"
      "{% if add_generation_prompt %}<|assistant|>{% endif %}{{ tools[0].function.name }}", context);
  const auto* spans = std::get_if<std::vector<TemplateSpan>>(&result);
  expect(spans != nullptr, "valid native template renders");
  if (!spans) return;
  std::string all;
  bool protected_input = false;
  for (const auto& span : *spans) {
    all += span.text;
    if (span.text.find("<|evil|>") != std::string::npos) protected_input = span.input;
  }
  expect(all == "<s><|user|><|evil|> hello<|assistant|>lookup", "messages, role, tools and generation prompt formatted");
  expect(protected_input, "message token literals stay input");
}
void transforms_keep_untrusted_text_marked() {
  for (const char* source : {"{{ tools | tojson }}", "{{ ('safe ' + messages[0].content) | replace('hello','world') }}",
                             "{{ [messages[0].content, 'suffix'] | join(' ') }}", "{{ messages[0].content | upper }}",
                             "{{ '' ~ messages }}", "{{ messages | upper }}",
                             "{{ strftime_now(messages[0].content) }}",
                             "{% for i in range(messages[0].content|length) %}{{ messages[0].content[i] }}{% endfor %}"}) {
    const auto result = render_jinja_template(jinja_helper_path(), source, context);
    const auto* spans = std::get_if<std::vector<TemplateSpan>>(&result);
    expect(spans != nullptr && !spans->empty(), "transform renders");
    if (spans) for (const auto& span : *spans) expect(span.input, "flattened/mixed transformed input is wholly untrusted");
  }
}
void malicious_templates_fail_without_host_access() {
  for (const char* source : {"{{ cycler.__init__.__globals__.os.popen('echo BAD').read() }}",
                             "{% include '/etc/passwd' %}", "{{ __import__('os').getenv('SECRET') }}",
                             "{{ raise_exception(messages[0].content) }}"}) {
    const auto result = render_jinja_template(jinja_helper_path(), source, context);
    expect(std::holds_alternative<TemplateRenderError>(result), "no import, host objects, files or exception output");
  }
}
void template_resource_limits_fail_closed() {
  for (const char* source : {"{{ 'x' * 1000000000 }}", "{% macro f() %}{{ f() }}{% endmacro %}{{ f() }}",
                             "{% for i in range(1000000000) %}x{% endfor %}"}) {
    const auto result = render_jinja_template(jinja_helper_path(), source, context);
    expect(std::holds_alternative<TemplateRenderError>(result), "memory/time/recursion violation fails closed");
  }
  expect(std::holds_alternative<TemplateRenderError>(render_jinja_template(jinja_helper_path(),
      std::string(kTemplateSourceMax + 1, 'x'), context)), "source bound checked before child");
}
} // namespace
int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"native_templates_render_messages_tools_and_mark_input", native_templates_render_messages_tools_and_mark_input},
      {"transforms_keep_untrusted_text_marked", transforms_keep_untrusted_text_marked},
      {"malicious_templates_fail_without_host_access", malicious_templates_fail_without_host_access},
      {"template_resource_limits_fail_closed", template_resource_limits_fail_closed}}, failures);
}
