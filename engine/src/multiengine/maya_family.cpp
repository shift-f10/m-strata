// src/multiengine/maya_family.cpp - multiengine_maya: Project Maya's GLM-5.3-Flash engine as a family library.
//
// The whole of Maya's `maya` program (third_party/maya/src/program/generate.cpp, not edited) compiled into this
// library, its main() under another name, and exported as maya_main: the launcher hands it the GLM command lines
// (--glm-pack of a glm5-next pack, a glm5-next GGUF), and it runs exactly as Maya's own maya executable does.
#define main maya_upstream_main
#include "../../third_party/maya/src/program/generate.cpp"
#undef main

#if defined(_WIN32)
extern "C" __declspec(dllexport) int maya_main(int argc, char** argv) { return maya_upstream_main(argc, argv); }
#else
extern "C" __attribute__((visibility("default"))) int maya_main(int argc, char** argv) {
    return maya_upstream_main(argc, argv);
}
#endif
