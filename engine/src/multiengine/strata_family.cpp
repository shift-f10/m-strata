// src/multiengine/strata_family.cpp - multiengine_strata: upstream Strata's engine as a family library of multiengine.
//
// The whole of upstream's engine program (src/program/generate.cpp, not edited) compiled into this library, its main()
// under another name, and exported as strata_main: the launcher hands it every command line that is not another
// family's, and it runs exactly as upstream's strata executable does (its own family switch, qwen35moe, included).
#define main strata_upstream_main
#include "../program/generate.cpp"
#undef main

#if defined(_WIN32)
extern "C" __declspec(dllexport) int strata_main(int argc, char** argv) { return strata_upstream_main(argc, argv); }
#else
extern "C" __attribute__((visibility("default"))) int strata_main(int argc, char** argv) {
    return strata_upstream_main(argc, argv);
}
#endif
