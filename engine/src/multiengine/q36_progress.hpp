// src/multiengine/q36_progress.hpp - the line multiengine_strata's Qwen3.6 engine prints while it reads its weights and
// places the routed experts (owner 2026-11-01).  Upstream's src/qwen35/q36_engine.cu says nothing between its context
// line and its placement line, and on a slow disk (the owner's Azure VM, about 21 MB/s: 4.40 GiB in 213.6 s, then some
// 13 minutes for 17 GB of experts) a start cut off by the server's start limit looked hung.  cmake/multiengine_qwen35.cmake
// builds our copy of upstream's file with this line, and upstream's advise_willneed read-ahead, put in.  Header-only.
#pragma once

#include <cstddef>
#include <cstdio>
#include <string>

namespace multiengine {

/// One line at most every `every_s` seconds, so a fast disk prints none.
class LoadProgress {
public:
    explicit LoadProgress(const char* what, double every_s = 10.0) : what_(what), every_s_(every_s) {}

    /// After layer `done` of `layers` (`bytes` read of `total`; total 0: not known), `now_s` seconds in: the line to print,
    /// or empty while the last one (or the start) is under every_s ago, and once every layer is done (the engine's own
    /// line says that).
    std::string step(double now_s, std::size_t done, std::size_t layers, std::size_t bytes, std::size_t total) {
        if (done >= layers || now_s - last_s_ < every_s_) return {};
        last_s_ = now_s;
        const double gib = 1024.0 * 1024.0 * 1024.0;
        char sizes[64];
        if (total > 0) std::snprintf(sizes, sizeof sizes, "%.2f of %.2f GiB", (double) bytes / gib, (double) total / gib);
        else std::snprintf(sizes, sizeof sizes, "%.2f GiB", (double) bytes / gib);
        char line[256];
        std::snprintf(line, sizeof line, "strata-q36: %s: %zu of %zu layers (%s) in %.0f s", what_, done, layers, sizes, now_s);
        return line;
    }

    /// The same, written to stderr when one is due.
    void say(double now_s, std::size_t done, std::size_t layers, std::size_t bytes, std::size_t total) {
        const std::string line = step(now_s, done, layers, bytes, total);
        if (!line.empty()) std::fprintf(stderr, "%s\n", line.c_str());
    }

private:
    const char* what_;
    double every_s_;
    double last_s_ = 0.0;
};

}  // namespace multiengine
