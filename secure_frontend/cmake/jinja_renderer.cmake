set(_jinja ${CMAKE_CURRENT_SOURCE_DIR}/third_party/llama_jinja)
add_library(secure_frontend_jinja_runtime STATIC
    ${_jinja}/jinja/lexer.cpp ${_jinja}/jinja/parser.cpp ${_jinja}/jinja/runtime.cpp
    ${_jinja}/jinja/value.cpp ${_jinja}/jinja/string.cpp ${_jinja}/unicode.cpp)
target_include_directories(secure_frontend_jinja_runtime PUBLIC ${_jinja})
target_compile_features(secure_frontend_jinja_runtime PUBLIC cxx_std_20)
secure_frontend_set_warnings(secure_frontend_jinja_runtime)
add_executable(strata-template src/template_main.cpp)
target_link_libraries(strata-template PRIVATE secure_frontend_jinja_runtime secure_frontend_json)
target_include_directories(strata-template PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(strata-template PRIVATE cxx_std_20)
secure_frontend_set_warnings(strata-template)
secure_frontend_add_library(secure_frontend_jinja_renderer SOURCES src/jinja_renderer.cpp
    LINKS secure_frontend_engine_stdio_process secure_frontend_json secure_frontend_paths)
add_dependencies(secure_frontend_jinja_renderer strata-template)
target_sources(secure_frontend_chat_generation_plan PRIVATE src/embedded_template_plan.cpp)
target_link_libraries(secure_frontend_chat_generation_plan PUBLIC secure_frontend_jinja_renderer)
add_dependencies(strata-unified-server strata-template)
install(TARGETS strata-template RUNTIME DESTINATION . COMPONENT runtime)
install(FILES ${_jinja}/LICENSE DESTINATION licenses/llama-jinja COMPONENT runtime)
if(STRATA_BUILD_SECURE_FRONTEND_TESTS)
    secure_frontend_add_test_executable(secure_frontend_jinja_renderer_tests tests/jinja_renderer_test.cpp
        secure_frontend_jinja_renderer)
    secure_frontend_add_cases(secure_frontend_jinja_renderer_tests
        native_templates_render_messages_tools_and_mark_input
        transforms_keep_untrusted_text_marked
        malicious_templates_fail_without_host_access
        template_resource_limits_fail_closed
        TIMEOUT 30)
endif()
