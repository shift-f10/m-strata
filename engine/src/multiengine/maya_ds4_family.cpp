// src/multiengine/maya_ds4_family.cpp - multiengine_maya_ds4: Project Maya's DeepSeek-V4.1-Flash engine as a family
// library.
//
// The whole of Maya's `maya-ds4` program (third_party/maya/src/ds4/ds4_main.cpp, not edited; the rest of its sources
// beside it in this library) compiled with its main() under another name, and exported as maya_ds4_main: the launcher
// hands it the command lines of a DeepSeek pack (--pack <dir> holding ds4.txt), and it runs exactly as Maya's own
// maya-ds4 executable does.
#define main maya_ds4_upstream_main
#include "../../third_party/maya/src/ds4/ds4_main.cpp"
#undef main

#if defined(_WIN32)
extern "C" __declspec(dllexport) int maya_ds4_main(int argc, char** argv) { return maya_ds4_upstream_main(argc, argv); }
#else
extern "C" __attribute__((visibility("default"))) int maya_ds4_main(int argc, char** argv) {
    return maya_ds4_upstream_main(argc, argv);
}
#endif
