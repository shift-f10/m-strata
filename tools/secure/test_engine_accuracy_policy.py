"""The shipped engine applies its accuracy policy before loading a family."""

import os
from pathlib import Path
import shutil
import subprocess
import sys

import pytest


ROOT = Path(__file__).resolve().parents[2]
SAFE = {
    "ROUTE_TAIL_SKIP": "0",
    "ROUTE_RESIDENT": "0",
    "ROUTE_PRIOR": "0",
    "ROUTE_RESIDENT_MTP": "0",
    "GLM_TRUNK_KEEP": "8",
    "GLM_TRUNK_KEEP_MAP": "",
    "GLM_DISK_SKIP_FROM": "8",
    "GLM_DISK_SKIP_W": "0",
    "GLM_MISS_SKIP_W": "0",
    "GLM_MTP_KEEP": "8",
    "GLM_MTP_MISS": "1",
    "DS4_DISK_SKIP_W": "0",
    "DS4_RAM_SKIP_W": "0",
    "SPEC_PROB": "0",
    "GLM_DEFER_CPU": "0",
    "HC_REQ8": "0",
    "DS4_PROMPT_SKIP": "1",
}


@pytest.fixture(scope="module")
def accuracy_launcher(tmp_path_factory):
    compiler = shutil.which("c++")
    if sys.platform != "linux" or compiler is None:
        pytest.skip("the fake-family launcher integration uses a Linux C++ compiler")
    stage = tmp_path_factory.mktemp("accuracy-launcher")
    flags = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion", "-Wshadow", "-Werror"]
    subprocess.run([compiler, *flags, str(ROOT / "engine/src/multiengine/launcher.cpp"),
                    "-ldl", "-o", str(stage / "multiengine")], check=True)
    checks = "\n".join(
        f'    check("{prefix}{name}", "{value}");'
        for name, value in SAFE.items() for prefix in ("STRATA_", "MAYA_")
    )
    source = stage / "family.cpp"
    source.write_text('''#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
namespace {
int failures = 0;
void check(const char* name, const char* expected) {
    const char* override = std::getenv((std::string("EXPECT_") + name).c_str());
    if (override) expected = override;
    const char* value = std::getenv(name);
    if (std::strcmp(value ? value : "", expected) != 0) {
        std::fprintf(stderr, "unsafe policy: %s\\n", name);
        ++failures;
    }
}
// A family may cache settings in static initialization, before its entry runs.
struct Initializer { Initializer() {
    check("DS4_DSPARK_SAMPLE", "0");
    check("DS4_YOCO", "1");
    check("DS4_YOCO_KEEP", "0");
''' + checks + '''
} } initialize;
}
extern "C" int strata_main(int, char**) { return failures == 0 ? 0 : 1; }
''', encoding="utf-8")
    subprocess.run([compiler, *flags, "-fPIC", "-shared", str(source), "-o",
                    str(stage / "libmultiengine_strata.so")], check=True)
    return stage / "multiengine"


@pytest.mark.parametrize("hostile_environment", [False, True])
def test_engine_preserves_all_selected_experts_before_loading_family(accuracy_launcher, hostile_environment):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("STRATA_", "MAYA_"))}
    if hostile_environment:
        environment["DS4_DSPARK_SAMPLE"] = "1"
        environment["DS4_YOCO"] = "2"
        environment["DS4_YOCO_KEEP"] = "128"
        for name in SAFE:
            environment["STRATA_" + name] = "1"
            environment["MAYA_" + name] = "2"
    result = subprocess.run([str(accuracy_launcher)], env=environment, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr


def test_selected_speed_settings_reach_family_static_initializers(accuracy_launcher):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith(("STRATA_", "MAYA_", "MSTRATA_", "EXPECT_"))}
    environment["MSTRATA_SPEED_SETTINGS"] = "ROUTE_TAIL_SKIP=7;GLM_TRUNK_KEEP=4;DS4_DSPARK_SAMPLE=1"
    for name, value in {"STRATA_ROUTE_TAIL_SKIP": "7", "MAYA_ROUTE_TAIL_SKIP": "7",
                        "STRATA_GLM_TRUNK_KEEP": "4", "MAYA_GLM_TRUNK_KEEP": "4",
                        "DS4_DSPARK_SAMPLE": "1"}.items():
        environment["EXPECT_" + name] = value
    result = subprocess.run([str(accuracy_launcher)], env=environment, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    environment["MSTRATA_SPEED_SETTINGS"] = "ROUTE_TAIL_SKIP=-1"
    result = subprocess.run([str(accuracy_launcher)], env=environment, capture_output=True, text=True)
    assert result.returncode != 0
    assert "invalid speed settings" in result.stderr


def test_explicit_speed_settings_are_bounded_and_override_only_selected_defaults(tmp_path):
    compiler = shutil.which("c++")
    if sys.platform != "linux" or compiler is None:
        pytest.skip("requires a Linux C++ compiler")
    source = tmp_path / "speed.cpp"
    source.write_text(r'''#include "secure_frontend/engine_accuracy_policy.hpp"
#include <map>
#include <string>
int main() {
    std::map<std::string, std::string> env;
    auto set = [&](const char* k, const char* v) { env[k] = v; return true; };
    using secure_frontend::apply_engine_accuracy_policy;
    if (!apply_engine_accuracy_policy(set, "ROUTE_TAIL_SKIP=7;ROUTE_RESIDENT=0.125;GLM_TRUNK_KEEP=4;DS4_YOCO_KEEP=128")) return 1;
    if (env["STRATA_ROUTE_TAIL_SKIP"] != "7" || env["MAYA_ROUTE_TAIL_SKIP"] != "7" ||
        env["STRATA_ROUTE_RESIDENT"] != "0.125" || env["STRATA_GLM_TRUNK_KEEP"] != "4" ||
        env["DS4_YOCO_KEEP"] != "128" || env["STRATA_SPEC_PROB"] != "0" || env["STRATA_GLM_MTP_KEEP"] != "8") return 2;
    for (const auto bad : {"ROUTE_TAIL_SKIP=-1", "ROUTE_TAIL_SKIP=10", "ROUTE_TAIL_SKIP=1.5",
                           "ROUTE_RESIDENT=nan", "ROUTE_RESIDENT=inf", "ROUTE_RESIDENT=1.1",
                           "GLM_TRUNK_KEEP=0", "SPEC_PROB=2", "HOME=1", "ROUTE_TAIL_SKIP=1;",
                           "ROUTE_TAIL_SKIP=1;ROUTE_TAIL_SKIP=2", "ROUTE_TAIL_SKIP=1x"}) {
        env.clear();
        if (apply_engine_accuracy_policy(set, bad) || !env.empty()) return 3;
    }
    if (apply_engine_accuracy_policy([](const char*, const char*) { return false; }, "ROUTE_TAIL_SKIP=7")) return 4;
    return 0;
}
''', encoding="utf-8")
    executable = tmp_path / "speed"
    subprocess.run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "secure_frontend/include"),
                    str(source), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
