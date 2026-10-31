/* engine/sdk/include/strata/family_abi.h - the engine family plugin ABI, version 1.
 *
 * The owner (2026-10-30): "our engine should have a published way so that we or another user can use those guidelines
 * and create a library that we can just add later on and it will seamlessly integrate along with cards, settings, etc."
 * Design: docs/superpowers/specs/2026-10-30-engine-family-plugins-design.md.
 *
 * A family plugin is one library in the install's plugins folder, <engine folder>/../plugins/<id>/:
 * multiengine_<id>.dll on Windows, libmultiengine_<id>.so on Linux, beside its plugin.json (kind "engine"). The launcher
 * runs it as
 *
 *     multiengine --family <id> --family-sha256 <the library's SHA-256> <the family's own arguments ...>
 *
 * and, before calling anything in it, checks that the library's bytes have that SHA-256 (the server pins it when the
 * owner approves the plugin), that strata_family_info() exists, that its `size` covers this version's fields, that its
 * ABI major is this header's, and that its `id` is the folder's. It then takes the process-wide steps every family
 * runs after (stdout unbuffered, CUDA kernels loaded with the context, --gpu as the visible GPUs) and calls
 * strata_family_main() with argv[0] and the family's own arguments only (never --family or --family-sha256).
 * strata_family_main() speaks the wire protocol on stdin and stdout; its return value is the process's exit code.
 * The server starts a card of the family as
 *
 *     multiengine --family <id> --family-sha256 <hex> --serve <the card's declared flags> [--gpu <list>] --native <gguf>
 *
 * so strata_family_main() is handed --serve, the flags the plugin's manifest declares, --gpu and --native.
 *
 * Compatibility: a later minor version only appends fields to StrataFamilyInfo and only adds optional exports; a new
 * major version is a new STRATA_FAMILY_ABI_VERSION, which this launcher refuses.
 */
#ifndef STRATA_FAMILY_ABI_H
#define STRATA_FAMILY_ABI_H

#include <stdint.h>

#define STRATA_FAMILY_ABI_VERSION 1
#define STRATA_FAMILY_ABI_MINOR 0
/* What strata_family_info()'s `abi` holds: major * 100000 + minor (as strata_kq's ABI is numbered). */
#define STRATA_FAMILY_ABI (STRATA_FAMILY_ABI_VERSION * 100000 + STRATA_FAMILY_ABI_MINOR)

#if defined(_WIN32)
#define STRATA_FAMILY_EXPORT __declspec(dllexport)
#else
#define STRATA_FAMILY_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StrataFamilyInfo {
  uint32_t size;        /* sizeof(StrataFamilyInfo) as the library was built: later versions only append */
  uint32_t abi;         /* STRATA_FAMILY_ABI */
  const char* id;       /* the family's id: its folder's name and its manifest's "id" ([a-z][a-z0-9_]{0,31}) */
  const char* version;  /* the library's own version, for the About tab and reports */
} StrataFamilyInfo;

typedef const StrataFamilyInfo* (*strata_family_info_fn)(void);
typedef int (*strata_family_main_fn)(int argc, char** argv);

/* The launcher only looks these up; a plugin defines them (the host defines STRATA_FAMILY_HOST). */
#ifndef STRATA_FAMILY_HOST
STRATA_FAMILY_EXPORT const StrataFamilyInfo* strata_family_info(void);
STRATA_FAMILY_EXPORT int strata_family_main(int argc, char** argv);
#endif

#ifdef __cplusplus
}
#endif

#endif /* STRATA_FAMILY_ABI_H */
