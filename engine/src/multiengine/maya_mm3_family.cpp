// src/multiengine/maya_mm3_family.cpp - multiengine_maya_mm3: Project Maya's MiniMax-M3.1-Flash engine as a family
// library.
//
// The whole of Maya's `maya-mm3` program (third_party/maya/src/mm3/mm3_main.cpp, not edited; the rest of its sources
// beside it in this library) compiled with its main() under another name, and exported as maya_mm3_main: the launcher
// hands it the command lines of a MiniMax pack (--pack <dir> holding mm3.txt), and it runs exactly as Maya's own
// maya-mm3 executable does.
#define main maya_mm3_upstream_main
#include "../../third_party/maya/src/mm3/mm3_main.cpp"
#undef main

#if defined(_WIN32)
extern "C" __declspec(dllexport) int maya_mm3_main(int argc, char** argv) { return maya_mm3_upstream_main(argc, argv); }
#else
extern "C" __attribute__((visibility("default"))) int maya_mm3_main(int argc, char** argv) {
    return maya_mm3_upstream_main(argc, argv);
}
#endif
