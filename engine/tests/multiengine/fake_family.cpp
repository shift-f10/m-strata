// tests/multiengine/fake_family.cpp - a fake engine family plugin for the launcher's --family tests
// (docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md). The build makes it four ways:
//   FAKE_ID testfam, FAKE_ABI = STRATA_FAMILY_ABI     a plugin as it should be
//   FAKE_ABI = 200000                                 a later ABI major the launcher must refuse
//   FAKE_ID other than its folder                     an id the launcher must refuse
//   FAKE_NO_INFO                                      no strata_family_info: refused
// Its main prints the arguments it was handed (so the test sees --family and --family-sha256 never reach it) and the
// visible GPUs the launcher set from --gpu, then returns 7.
#include "strata/family_abi.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

std::string visible_gpus() {
#if defined(_WIN32)
  char* value = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&value, &length, "CUDA_VISIBLE_DEVICES") != 0 || value == nullptr) return {};
  std::string text(value);
  std::free(value);
  return text;
#else
  const char* value = std::getenv("CUDA_VISIBLE_DEVICES");
  return value != nullptr ? std::string(value) : std::string();
#endif
}

#if !defined(FAKE_NO_INFO)
// FAKE_ID is a bare name on the compiler's command line
#define FAKE_TEXT(name) #name
#define FAKE_STRING(name) FAKE_TEXT(name)
const StrataFamilyInfo kInfo{static_cast<uint32_t>(sizeof(StrataFamilyInfo)), FAKE_ABI, FAKE_STRING(FAKE_ID), "0.0.1-test"};
#endif

}  // namespace

#if !defined(FAKE_NO_INFO)
extern "C" STRATA_FAMILY_EXPORT const StrataFamilyInfo* strata_family_info(void) { return &kInfo; }
#endif

extern "C" STRATA_FAMILY_EXPORT int strata_family_main(int argc, char** argv) {
  std::printf("fake family: %d args:", argc - 1);
  for (int i = 1; i < argc; ++i) std::printf(" %s", argv[i]);
  std::printf("\nvisible gpus: %s\n", visible_gpus().c_str());
  std::fflush(stdout);
  return 7;
}
