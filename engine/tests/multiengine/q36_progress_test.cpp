// multiengine_strata's Qwen3.6 engine: its progress line while it reads its weights and places the routed experts
// (src/multiengine/q36_progress.hpp), and our copy of upstream's q36_engine.cu, which prints it (written at configure
// time by cmake/multiengine_qwen35.cmake, its path passed in).  No GPU, no model.
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "q36_progress.hpp"

static int failed = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failed; } } while (0)

int main(int argc, char** argv) {
    const std::size_t GiB = std::size_t{1} << 30;
    // the owner's VM: 41 layers of routed experts, 17 GiB, about 21 MB/s - one line every 10 s, none before
    multiengine::LoadProgress place("placing the routed experts");
    CHECK(place.step(4.0, 1, 41, GiB / 4, 17 * GiB).empty());
    CHECK(place.step(12.0, 3, 41, GiB, 17 * GiB) == "strata-q36: placing the routed experts: 3 of 41 layers (1.00 of 17.00 GiB) in 12 s");
    CHECK(place.step(21.0, 4, 41, GiB + GiB / 4, 17 * GiB).empty());   // 9 s after the last one
    CHECK(place.step(22.0, 5, 41, 2 * GiB, 17 * GiB) == "strata-q36: placing the routed experts: 5 of 41 layers (2.00 of 17.00 GiB) in 22 s");
    CHECK(place.step(400.0, 41, 41, 17 * GiB, 17 * GiB).empty());       // every layer done: the engine's own line says so
    // the weights' read, whose total is not known ahead: the bytes so far only
    multiengine::LoadProgress load("reading the weights");
    CHECK(load.step(30.0, 6, 41, GiB / 2, 0) == "strata-q36: reading the weights: 6 of 41 layers (0.50 GiB) in 30 s");
    // a fast disk: everything inside the first 10 s, no line at all
    multiengine::LoadProgress fast("placing the routed experts", 10.0);
    for (std::size_t l = 1; l <= 41; ++l) CHECK(fast.step(0.2 * (double) l, l, 41, l * GiB / 2, 21 * GiB).empty());
    // our copy of upstream's engine: both lines, and the read-ahead before each tensor's and each layer's experts' copy
    CHECK(argc == 2);
    if (argc == 2) {
        std::ifstream in(argv[1], std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        const std::string src = text.str();
        const auto count = [&src](const std::string& part) {
            std::size_t n = 0;
            for (std::size_t at = src.find(part); at != std::string::npos; at = src.find(part, at + 1)) ++n;
            return n;
        };
        CHECK(count("#include \"q36_progress.hpp\"") == 1);
        CHECK(count("me_progress(\"reading the weights\")") == 1);
        CHECK(count("me_progress(\"placing the routed experts\")") == 1);
        CHECK(count("strata::platform::advise_willneed(gm.shard(s).tensor_data(*t)") == 3);   // f32, mat, the embedding
        CHECK(count("me_advise_experts(") == 4);   // its definition, the three placements
        CHECK(count("me_placed_layer(") == 3);     // the three placements
    }
    if (failed == 0) std::printf("q36_progress_test: ok\n");
    return failed == 0 ? 0 : 1;
}
