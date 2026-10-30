// src/multiengine/family_plugin.cpp - see family_plugin.hpp.
#include "family_plugin.hpp"

#define STRATA_FAMILY_HOST
#include "strata/family_abi.h"

#include "../prepare/sha256.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace multiengine {
namespace {

bool hex_digit(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return text;
}

// A refusal: said on stderr, the process's exit code 1.
int refuse(const std::string& id, const char* why) {
    std::fprintf(stderr, "multiengine: family %s refused: %s\n", id.c_str(), why);
    return 1;
}

#if defined(_WIN32)
std::wstring library_path(const std::string& id) {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    std::wstring dir = n == 0 || n >= buf.size() ? std::wstring(L".") : std::wstring(buf.data(), n);
    const size_t cut = dir.find_last_of(L"\\/");
    dir = cut == std::wstring::npos ? std::wstring(L".") : dir.substr(0, cut);
    const std::wstring wide(id.begin(), id.end());   // an id is ASCII
    return dir + L"\\..\\plugins\\" + wide + L"\\multiengine_" + wide + L".dll";
}
#else
std::string library_path(const std::string& id) {
    std::vector<char> buf(4096);
    const ssize_t n = readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    std::string dir = n <= 0 ? std::string(".") : std::string(buf.data(), static_cast<size_t>(n));
    const size_t cut = dir.find_last_of('/');
    dir = cut == std::string::npos ? std::string(".") : dir.substr(0, cut);
    return dir + "/../plugins/" + id + "/libmultiengine_" + id + ".so";
}
#endif

}  // namespace

bool valid_family_id(const std::string& id) {
    if (id.empty() || id.size() > 32U || id[0] < 'a' || id[0] > 'z') return false;
    for (const char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

std::variant<std::monostate, PluginRequest, std::string> parse_plugin_request(const int argc, char** argv) {
    if (argc < 2 || std::strcmp(argv[1], "--family") != 0) return std::monostate{};
    if (argc < 3) return std::string("--family needs a family id");
    PluginRequest request;
    request.id = argv[2];
    if (!valid_family_id(request.id)) {
        return std::string("the family id must be a lowercase letter, then up to 31 lowercase letters, digits or '_'");
    }
    if (argc < 5 || std::strcmp(argv[3], "--family-sha256") != 0) {
        return std::string("--family <id> must be followed by --family-sha256 <the library's SHA-256>");
    }
    const std::string sum = argv[4];
    bool hex = sum.size() == 64U;
    for (const char c : sum) hex = hex && hex_digit(c);
    if (!hex) return std::string("--family-sha256 takes the library's SHA-256 as 64 hex digits");
    request.sha256 = lower(sum);
    request.first = 5;
    return request;
}

int run_plugin(const PluginRequest& request, const int argc, char** argv, void (*steps)(int, char**)) {
    // the family's argv: the launcher's argv[0], then its own arguments only
    std::vector<char*> args{argv[0]};
    for (int i = request.first; i < argc; ++i) args.push_back(argv[i]);
    args.push_back(nullptr);
    const int count = static_cast<int>(args.size()) - 1;

    strata_prepare::Sha256 hash;
    std::vector<unsigned char> block(1U << 20U);
#if defined(_WIN32)
    const std::wstring path = library_path(request.id);
    // read sharing only: the file cannot be written, renamed or deleted until it is loaded and this handle is closed
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) return refuse(request.id, "cannot open its library in plugins\\<id>\\");
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, block.data(), static_cast<DWORD>(block.size()), &got, nullptr)) {
            CloseHandle(file);
            return refuse(request.id, "its library could not be read");
        }
        if (got == 0) break;
        hash.update(block.data(), got);
    }
    if (hash.hex() != request.sha256) {
        CloseHandle(file);
        return refuse(request.id, "its library's SHA-256 is not the one approved for it");
    }
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    CloseHandle(file);
    if (module == nullptr) return refuse(request.id, "its library, or a library it needs, could not be loaded");
    const auto info_fn = reinterpret_cast<strata_family_info_fn>(reinterpret_cast<void*>(GetProcAddress(module, "strata_family_info")));
    const auto main_fn = reinterpret_cast<strata_family_main_fn>(reinterpret_cast<void*>(GetProcAddress(module, "strata_family_main")));
#else
    const std::string path = library_path(request.id);
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return refuse(request.id, "cannot open its library in plugins/<id>/");
    for (;;) {
        const ssize_t got = read(fd, block.data(), block.size());
        if (got < 0) {
            close(fd);
            return refuse(request.id, "its library could not be read");
        }
        if (got == 0) break;
        hash.update(block.data(), static_cast<size_t>(got));
    }
    if (hash.hex() != request.sha256) {
        close(fd);
        return refuse(request.id, "its library's SHA-256 is not the one approved for it");
    }
    // loaded by its path (so the library's $ORIGIN is its folder), then checked to be the very file hashed: the same
    // device and inode as the descriptor still open (the folder is owner-only: only the owner could swap it between)
    struct stat hashed {};
    struct stat loaded {};
    const bool known = fstat(fd, &hashed) == 0;
    void* module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    const bool same = known && stat(path.c_str(), &loaded) == 0 && loaded.st_dev == hashed.st_dev && loaded.st_ino == hashed.st_ino;
    close(fd);
    if (module == nullptr) return refuse(request.id, "its library, or a library it needs, could not be loaded");
    if (!same) {
        dlclose(module);
        return refuse(request.id, "its library changed while it was checked");
    }
    const auto info_fn = reinterpret_cast<strata_family_info_fn>(dlsym(module, "strata_family_info"));
    const auto main_fn = reinterpret_cast<strata_family_main_fn>(dlsym(module, "strata_family_main"));
#endif
    if (info_fn == nullptr) return refuse(request.id, "its library has no strata_family_info");
    if (main_fn == nullptr) return refuse(request.id, "its library has no strata_family_main");
    const StrataFamilyInfo* info = info_fn();
    if (info == nullptr || info->size < sizeof(StrataFamilyInfo)) return refuse(request.id, "its strata_family_info is too short");
    if (info->abi / 100000U != STRATA_FAMILY_ABI_VERSION) {
        return refuse(request.id, "its library is built for another family ABI version");
    }
    if (info->id == nullptr || request.id != info->id) return refuse(request.id, "its library says another family id");
    steps(count, args.data());
    return main_fn(count, args.data());
}

}  // namespace multiengine
