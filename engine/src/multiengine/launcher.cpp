// src/multiengine/launcher.cpp - multiengine(.exe): the one engine executable, and nothing but the switch between
// its model families.
//
// Each family is its own library beside the executable, compiled from its own sources, so no family - and no update
// of one - can change what another computes:
//   multiengine_strata    upstream Strata's engine, src/program/generate.cpp unchanged (qwen4exp, qwen35moe)
//   multiengine_maya      Project Maya's `maya` program, third_party/maya unchanged (glm5-next: GLM-5.3-Flash)
//   multiengine_maya_ds4  Project Maya's `maya-ds4` program (deepseek41: DeepSeek-V4.1-Flash)
//   multiengine_maya_mm3  Project Maya's `maya-mm3` program (minimax-m3: MiniMax-M3.1-Flash)
//   multiengine_raon1     raon1 on our port of Maya's GLM engine, third_party/project-maya (raon1)
//   multiengine_gemma4    Gemma 4 26B-A4B on its own copy of the Strata core, src/gemma4 (gemma4)
//   multiengine_flint     flint on the same pieces as Gemma 4, src/flint (flint)
// The launcher reads which one a command line is for, loads only that library and hands it the whole command line:
//   --glm-pack DIR   the architecture of the GGUF the pack was made from (its native_experts.txt names it): raon1
//                    goes to multiengine_raon1, any other to Maya's GLM program
//   --native / --model GGUF  the general.architecture of its first shard, the key upstream's family switch reads
//   --pack DIR       alone: a DeepSeek pack (ds4.txt) or a MiniMax pack (mm3.txt), each read by its Maya program
//   anything else    upstream Strata's engine, which also reports a file the launcher could not read.
// Maya's programs take their own process steps, as Maya's executables do, so the launcher leaves the process to them.
// For the others it first takes the process-wide steps upstream Strata's main() takes ahead of its family switch, which
// the Gemma 4, flint and raon1 entries were written to run after: stdout unbuffered, every CUDA kernel loaded with the
// context, --gpu as the visible GPUs.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// dbghelp.h needs windows.h first
#include <dbghelp.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace {

using Entry = int (*)(int, char**);

std::string env(const char* name) {
#if defined(_WIN32)
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || v == nullptr) return {};
    std::string s(v);
    std::free(v);
    return s;
#else
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
#endif
}

bool has_env(const char* name) {
#if defined(_WIN32)
    char* v = nullptr;
    size_t n = 0;
    const bool set = _dupenv_s(&v, &n, name) == 0 && v != nullptr;
    std::free(v);
    return set;
#else
    return std::getenv(name) != nullptr;
#endif
}

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// ---- which family: the architecture of a GGUF -------------------------------------------------------------------

// The shard that holds the metadata (00001) of a split model, named as llama.cpp's gguf-split names the shards:
// `<name>-00001-of-0000N.gguf` ... (upstream's gguf_split_paths, which the old family switch read through).  Empty
// when a shard is missing: that model goes to upstream's engine, which names the missing shard.
std::string first_shard(const std::string& any) {
    static const char tag[] = "-of-";
    const size_t at = any.rfind(tag);
    if (at == std::string::npos || at < 6 || any[at - 6] != '-' || any.size() != at + 4 + 5 + 5 ||
        any.compare(at + 9, 5, ".gguf") != 0)
        return any;
    int no = 0, n = 0;
    for (size_t i = 0; i < 5; ++i) {
        const char a = any[at - 5 + i], b = any[at + 4 + i];
        if (a < '0' || a > '9' || b < '0' || b > '9') return any;
        no = no * 10 + (a - '0');
        n = n * 10 + (b - '0');
    }
    if (n < 1 || no < 1 || no > n) return any;
    std::string first;
    for (int i = 1; i <= n; ++i) {
        char num[12];   // any int's digits (gcc -Wformat-truncation cannot see that i <= 99999 here)
        std::snprintf(num, sizeof num, "%05d", i);
        std::string p = any;
        p.replace(at - 5, 5, num);
        if (!std::ifstream(p, std::ios::binary)) return {};
        if (i == 1) first = p;
    }
    return first;
}

// general.architecture from a GGUF header (version 2 or 3, little-endian), or empty when the file is not one or the
// key is missing.  Every length is bounded before it is used; the key is the header's first in practice, but any
// position is read correctly.
std::string gguf_architecture(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    const auto get = [&f](auto& v) { return static_cast<bool>(f.read(reinterpret_cast<char*>(&v), sizeof v)); };
    const auto skip = [&f](uint64_t n) {
        if (n > static_cast<uint64_t>(INT64_MAX)) return false;
        return static_cast<bool>(f.seekg(static_cast<std::streamoff>(n), std::ios::cur));
    };
    uint32_t magic = 0, version = 0;
    uint64_t n_tensors = 0, n_kv = 0;
    if (!get(magic) || magic != 0x46554747u || !get(version) || version < 2 || version > 3 || !get(n_tensors) ||
        !get(n_kv))
        return {};
    constexpr uint32_t kString = 8, kArray = 9;
    // bytes of each scalar type: u8 i8 u16 i16 u32 i32 f32 bool (string) (array) u64 i64 f64
    static const uint8_t kSize[13] = {1, 1, 2, 2, 4, 4, 4, 1, 0, 0, 8, 8, 8};
    const auto scalar = [&](uint32_t t) -> uint64_t { return t < 13 ? kSize[t] : 0; };
    for (uint64_t i = 0; i < n_kv && i < (1u << 20); ++i) {
        uint64_t klen = 0;
        if (!get(klen) || klen == 0 || klen > 65536) return {};
        std::string key(static_cast<size_t>(klen), '\0');
        uint32_t type = 0;
        if (!f.read(key.data(), static_cast<std::streamsize>(klen)) || !get(type)) return {};
        if (key == "general.architecture") {
            uint64_t len = 0;
            if (type != kString || !get(len) || len > 256) return {};
            std::string arch(static_cast<size_t>(len), '\0');
            if (!f.read(arch.data(), static_cast<std::streamsize>(len))) return {};
            return arch;
        }
        if (type == kString) {
            uint64_t len = 0;
            if (!get(len) || !skip(len)) return {};
        } else if (type == kArray) {
            uint32_t et = 0;
            uint64_t n = 0;
            if (!get(et) || !get(n)) return {};
            if (et == kString) {
                for (uint64_t j = 0; j < n; ++j) {
                    uint64_t len = 0;
                    if (!get(len) || !skip(len)) return {};
                }
            } else {
                const uint64_t es = scalar(et);
                if (es == 0 || n > (UINT64_C(1) << 56) || !skip(n * es)) return {};
            }
        } else {
            const uint64_t s = scalar(type);
            if (s == 0 || !skip(s)) return {};
        }
    }
    return {};
}

#if defined(MULTIENGINE_HAVE_MAYA) || defined(MULTIENGINE_HAVE_RAON1)
// The value after the first `flag` on the command line, or nullptr.
const char* flag_value(int argc, char** argv, const char* flag) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return nullptr;
}

bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}
#endif

// The architecture of the model a command line runs: the first --native / --model value, as upstream reads it.
std::string model_architecture(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--native") != 0 && std::strcmp(argv[i], "--model") != 0) continue;
        const std::string first = first_shard(argv[i + 1]);
        return first.empty() ? std::string() : gguf_architecture(first);
    }
    return {};
}

#if defined(MULTIENGINE_HAVE_MAYA) || defined(MULTIENGINE_HAVE_RAON1)
// The architecture of a GLM-engine pack (--glm-pack DIR): the GGUF it was made from, as the GLM engines find it - the
// first line of DIR/native_experts.txt names the first shard ("... absolute offsets in <name>, ..."), which sits beside
// the pack directory.  Empty when either cannot be read.
std::string pack_architecture(const std::string& dir) {
    std::ifstream f(dir + "/native_experts.txt");
    std::string line;
    if (!f || !std::getline(f, line) || line.size() > 4096 || line.rfind("#", 0) != 0) return {};
    static const char key[] = "absolute offsets in ";
    const size_t at = line.find(key);
    if (at == std::string::npos) return {};
    const size_t from = at + sizeof key - 1;
    const size_t comma = line.find(',', from);
    std::string name = line.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
    if (!name.empty() && name.back() == '\r') name.pop_back();
    return name.empty() ? std::string() : gguf_architecture(dir + "/../" + name);
}
#endif

#if defined(MULTIENGINE_HAVE_MAYA)
bool file_exists(const std::string& path) { return static_cast<bool>(std::ifstream(path, std::ios::binary)); }
#endif

// ---- the switch ---------------------------------------------------------------------------------------------------

struct Family {
    const char* lib;
    const char* entry;
    bool own_process_steps;   // Maya's programs: they set up the process themselves, as Maya's executables do
};

Family family_for(int argc, char** argv) {
#if defined(MULTIENGINE_HAVE_RAON1)
    const Family raon1{"multiengine_raon1", "glm_main", false};
#endif
#if defined(MULTIENGINE_HAVE_MAYA)
    const Family maya_glm{"multiengine_maya", "maya_main", true};
#endif
#if defined(MULTIENGINE_HAVE_MAYA) || defined(MULTIENGINE_HAVE_RAON1)
    // a GLM-engine pack: no --native to read, so the pack's own source GGUF says which
    if (has_flag(argc, argv, "--glm-pack")) {
        const char* dir = flag_value(argc, argv, "--glm-pack");
        const std::string arch = dir != nullptr ? pack_architecture(dir) : std::string();
#if defined(MULTIENGINE_HAVE_RAON1)
        if (arch == "raon1") return raon1;
#endif
#if defined(MULTIENGINE_HAVE_MAYA)
        return maya_glm;
#else
        return raon1;
#endif
    }
#endif
    const std::string arch = model_architecture(argc, argv);
#if defined(MULTIENGINE_HAVE_GEMMA4)
    if (arch == "gemma4") return {"multiengine_gemma4", "gemma4_main", false};
#endif
#if defined(MULTIENGINE_HAVE_FLINT)
    if (arch == "flint") return {"multiengine_flint", "flint_main", false};
#endif
#if defined(MULTIENGINE_HAVE_RAON1)
    if (arch == "raon1") return raon1;
#endif
#if defined(MULTIENGINE_HAVE_MAYA)
    if (arch == "glm5-next") return maya_glm;
    // Maya's DeepSeek and MiniMax programs read a pack directory alone, which their own geometry file marks
    if (flag_value(argc, argv, "--native") == nullptr && flag_value(argc, argv, "--model") == nullptr) {
        if (const char* dir = flag_value(argc, argv, "--pack")) {
            if (file_exists(std::string(dir) + "/ds4.txt")) return {"multiengine_maya_ds4", "maya_ds4_main", true};
            if (file_exists(std::string(dir) + "/mm3.txt")) return {"multiengine_maya_mm3", "maya_mm3_main", true};
        }
    }
#elif defined(MULTIENGINE_HAVE_RAON1)
    if (arch == "glm5-next") return raon1;
#endif
    return {"multiengine_strata", "strata_main", false};
}

// ---- the family libraries: beside the executable ------------------------------------------------------------------

#if defined(_WIN32)
std::wstring exe_dir() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return L".";
    std::wstring dir(buf.data(), n);
    const size_t cut = dir.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring(L".") : dir.substr(0, cut);
}
#else
std::string exe_dir() {
    std::vector<char> buf(4096);
    const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return ".";
    std::string dir(buf.data(), static_cast<size_t>(n));
    const size_t cut = dir.find_last_of('/');
    return cut == std::string::npos ? std::string(".") : dir.substr(0, cut);
}
#endif

// The family's entry, from <exe folder>/<lib>.dll (lib<lib>.so); its own dependencies (the CUDA libraries) are found
// from there and then by the system's usual search.  nullptr after saying why.
Entry load_entry(const char* lib, const char* symbol) {
#if defined(_WIN32)
    std::wstring path = exe_dir() + L"\\";
    for (const char* c = lib; *c != 0; ++c) path += static_cast<wchar_t>(*c);
    path += L".dll";
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h == nullptr) {
        const DWORD e = GetLastError();
        std::fprintf(stderr,
                     "multiengine: cannot load %s.dll from the engine's folder (Windows error %lu%s)\n", lib,
                     static_cast<unsigned long>(e),
                     e == ERROR_MOD_NOT_FOUND ? ": it, or a library it needs such as the CUDA runtime's, is missing" : "");
        return nullptr;
    }
    const FARPROC p = GetProcAddress(h, symbol);
    if (p == nullptr) {
        std::fprintf(stderr, "multiengine: %s.dll has no %s\n", lib, symbol);
        return nullptr;
    }
    return reinterpret_cast<Entry>(reinterpret_cast<void*>(p));
#else
    const std::string path = exe_dir() + "/lib" + lib + ".so";
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (h == nullptr) {
        std::fprintf(stderr, "multiengine: cannot load %s: %s\n", path.c_str(), dlerror());
        return nullptr;
    }
    void* p = dlsym(h, symbol);
    if (p == nullptr) {
        std::fprintf(stderr, "multiengine: %s has no %s\n", path.c_str(), symbol);
        return nullptr;
    }
    return reinterpret_cast<Entry>(p);
#endif
}

int run_family(const char* lib, const char* symbol, int argc, char** argv) {
    const Entry entry = load_entry(lib, symbol);
    return entry != nullptr ? entry(argc, argv) : 1;
}

#if defined(MULTIENGINE_HAVE_MAYA)
// Whether <exe folder>/<lib>.dll (lib<lib>.so) is there: Maya's build leaves a program out where the GPU target cannot
// run it (maya-mm3 below sm_70), so --families names a Maya family only when its library was built.
bool library_present(const char* lib) {
#if defined(_WIN32)
    std::wstring path = exe_dir() + L"\\";
    for (const char* c = lib; *c != 0; ++c) path += static_cast<wchar_t>(*c);
    path += L".dll";
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return file_exists(exe_dir() + "/lib" + lib + ".so");
#endif
}
#endif

// The process-wide steps upstream Strata's main() takes ahead of its family switch, for the families written to run
// after them (upstream's engine, Gemma 4, raon1).
void strata_process_steps(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // Load every CUDA kernel when the context is created, before an expert cache takes the free VRAM (upstream's
    // reason: a kernel first used mid-prompt found no VRAM left for its code).  The caller's own setting wins.
    if (!has_env("CUDA_MODULE_LOADING")) set_env("CUDA_MODULE_LOADING", "EAGER");
    // --gpu LIST pins the visible GPUs (nvidia-smi / PCI order): CUDA_VISIBLE_DEVICES is read at the first CUDA call,
    // so it is set here, before the family library is loaded.  Each family reads the value again from its options.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--gpu") != 0) continue;
        const std::string prev = env("CUDA_VISIBLE_DEVICES");
        if (!prev.empty() && prev != argv[i + 1])
            std::fprintf(stderr, "warning: --gpu %s overrides CUDA_VISIBLE_DEVICES=%s from the environment\n", argv[i + 1],
                         prev.c_str());
        set_env("CUDA_DEVICE_ORDER", "PCI_BUS_ID");
        set_env("CUDA_VISIBLE_DEVICES", argv[i + 1]);
        break;
    }
}

#if defined(_WIN32)
// A last-resort stack trace on an otherwise-silent crash, in whichever family it happens: symbolize the faulting
// thread's frames against the PDBs (beside the libraries, or where they were linked) and print them to stderr before
// the process dies.
LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0) { Sleep(4000); return EXCEPTION_EXECUTE_HANDLER; }   // first faulting thread only (dbghelp is not thread-safe)
    const HANDLE proc = GetCurrentProcess();
    const HANDLE thr = GetCurrentThread();
    char dir[MAX_PATH * 2] = {};
    const DWORD dn = GetModuleFileNameA(nullptr, dir, static_cast<DWORD>(sizeof dir));
    if (dn > 0 && dn < sizeof dir) {
        char* cut = std::strrchr(dir, '\\');
        if (cut != nullptr) *cut = 0;
    } else {
        dir[0] = 0;
    }
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(proc, dir[0] != 0 ? dir : nullptr, TRUE);
    std::fprintf(stderr, "\n=== STRATA CRASH: exception 0x%08lx at %p ===\n",
                 static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode), ep->ExceptionRecord->ExceptionAddress);
    char sbuf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(sbuf);
    const auto emit = [&](DWORD64 a, int i) {
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char* name = SymFromAddr(proc, a, &disp, sym) ? sym->Name : "(unknown)";
        IMAGEHLP_LINE64 line;
        line.SizeOfStruct = sizeof(line);
        DWORD ld = 0;
        IMAGEHLP_MODULE64 mod;
        mod.SizeOfStruct = sizeof(mod);
        const char* modn = SymGetModuleInfo64(proc, a, &mod) ? mod.ModuleName : "?";
        if (SymGetLineFromAddr64(proc, a, &ld, &line))
            std::fprintf(stderr, "  [%2d] %s!%s  (%s:%lu)\n", i, modn, name, line.FileName,
                         static_cast<unsigned long>(line.LineNumber));
        else
            std::fprintf(stderr, "  [%2d] %s!%s (+0x%llx) @%p\n", i, modn, name, static_cast<unsigned long long>(disp),
                         reinterpret_cast<void*>(a));
    };
    emit(static_cast<DWORD64>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress)), -1);   // the faulting instruction
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 fr;
    std::memset(&fr, 0, sizeof(fr));
    fr.AddrPC.Offset = ctx.Rip;    fr.AddrPC.Mode = AddrModeFlat;
    fr.AddrFrame.Offset = ctx.Rbp; fr.AddrFrame.Mode = AddrModeFlat;
    fr.AddrStack.Offset = ctx.Rsp; fr.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 64; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thr, &fr, &ctx, nullptr, SymFunctionTableAccess64,
                         SymGetModuleBase64, nullptr) ||
            fr.AddrPC.Offset == 0)
            break;
        emit(fr.AddrPC.Offset, i);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(crash_filter);
#endif
    // `--version` / `-V`: upstream's engine answers (its version and nothing else, before any GPU is touched).
    // `--families`: the GGUF architectures this build runs, every family's.
    for (int i = 1; i < argc && i < 3; ++i) {
        if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-V") == 0) {
            std::setvbuf(stdout, nullptr, _IONBF, 0);
            return run_family("multiengine_strata", "strata_main", argc, argv);
        }
        if (std::strcmp(argv[i], "--families") == 0) {
            std::string fams = "qwen4exp";
#if defined(MULTIENGINE_HAVE_QWEN35)
            fams += " qwen35moe";
#endif
#if defined(MULTIENGINE_HAVE_MAYA)
            if (library_present("multiengine_maya")) fams += " glm5-next";
            if (library_present("multiengine_maya_ds4")) fams += " deepseek41";
            if (library_present("multiengine_maya_mm3")) fams += " minimax-m3";
#elif defined(MULTIENGINE_HAVE_RAON1)
            fams += " glm5-next";
#endif
#if defined(MULTIENGINE_HAVE_RAON1)
            fams += " raon1";
#endif
#if defined(MULTIENGINE_HAVE_GEMMA4)
            fams += " gemma4";
#endif
#if defined(MULTIENGINE_HAVE_FLINT)
            fams += " flint";
#endif
            std::printf("%s\n", fams.c_str());
            return 0;
        }
    }
    const Family family = family_for(argc, argv);
    if (!family.own_process_steps) strata_process_steps(argc, argv);
    return run_family(family.lib, family.entry, argc, argv);
}
