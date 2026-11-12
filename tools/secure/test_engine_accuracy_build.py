"""Generated build commands preserve CUDA accuracy without editing vendor trees."""

import json
from pathlib import Path
import shlex
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
POLICY = ROOT / "engine/cmake/multiengine_accuracy.cmake"


@pytest.mark.parametrize("fast_math", [None, False, True])
def test_generated_commands_remove_only_fast_math(tmp_path, fast_math):
    cmake = shutil.which("cmake")
    compiler = shutil.which("c++")
    if not cmake or not compiler:
        pytest.skip("requires CMake and a C++ compiler")
    (tmp_path / "child").mkdir()
    (tmp_path / "one.cpp").write_text("int one() { return 1; }\n")
    (tmp_path / "child/two.cpp").write_text("int two() { return 2; }\n")
    (tmp_path / "child/CMakeLists.txt").write_text('''
add_library(child OBJECT two.cpp ../one.cpp)
set_property(SOURCE two.cpp APPEND PROPERTY COMPILE_OPTIONS
  "$<IF:$<BOOL:0>,,--use_fast_math>" "-fmad=true")
set_property(SOURCE ../one.cpp APPEND PROPERTY COMPILE_OPTIONS "-use_fast_math")
target_compile_options(child PRIVATE "-DCHILD_KEEP=1"
  $<$<COMPILE_LANGUAGE:CXX>:-DGEN_KEEP=1 -use_fast_math>)
''')
    (tmp_path / "CMakeLists.txt").write_text('''
cmake_minimum_required(VERSION 3.20)
project(accuracy_fixture LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_library(root OBJECT one.cpp)
target_compile_options(root PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:--extended-lambda -use_fast_math>"
  "-Werror" "-DKEEP_FAST_MATH_WORD=use_fast_math")
set_property(SOURCE one.cpp APPEND PROPERTY COMPILE_OPTIONS "--use_fast_math")
add_subdirectory(child)
if(APPLY_ACCURACY_POLICY)
  include("''' + POLICY.as_posix() + '''" OPTIONAL)
  if(COMMAND multiengine_preserve_cuda_accuracy)
    multiengine_preserve_cuda_accuracy("${CMAKE_CURRENT_SOURCE_DIR}")
    # Applying twice must not damage generator expressions or preserved flags.
    multiengine_preserve_cuda_accuracy("${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endif()
''')
    commands = {}
    for apply in (False, True):
        build = tmp_path / ("with_policy" if apply else "baseline")
        math_option = [] if fast_math is None else ["-DMULTIENGINE_FAST_MATH=" + ("ON" if fast_math else "OFF")]
        subprocess.run([cmake, "-S", str(tmp_path), "-B", str(build),
                        "-G", "Unix Makefiles", "-DCMAKE_CXX_COMPILER=" + compiler,
                        "-DAPPLY_ACCURACY_POLICY=" + ("ON" if apply else "OFF"), *math_option],
                       check=True, capture_output=True, text=True)
        commands[apply] = json.loads((build / "compile_commands.json").read_text())
    assert len(commands[True]) == len(commands[False]) == 3
    before = "\n".join(row["command"] for row in commands[False])
    after = "\n".join(row["command"] for row in commands[True])
    assert "--use_fast_math" in before and "-use_fast_math" in before
    assert ("-use_fast_math" in after) == bool(fast_math)
    for preserved in ("--extended-lambda", "-fmad=true", "-Werror",
                      "-DKEEP_FAST_MATH_WORD=use_fast_math", "-DCHILD_KEEP=1",
                      "-DGEN_KEEP=1"):
        assert preserved in after
    if fast_math:
        assert after == before  # opt-in retains every upstream target/source flag exactly
    else:
        assert "--extended-lambda" in shlex.split(commands[True][0]["command"])


def test_both_engine_overlays_apply_accuracy_policy():
    for name in ("multiengine.cmake", "multiengine_maya.cmake"):
        source = (ROOT / "engine/cmake" / name).read_text()
        assert "multiengine_accuracy.cmake" in source
        assert 'multiengine_preserve_cuda_accuracy("${CMAKE_CURRENT_SOURCE_DIR}")' in source


def test_maya_external_configure_tracks_overlay_inputs():
    source = (ROOT / "engine/cmake/multiengine.cmake").read_text()
    dependency_call = source.split(
        "ExternalProject_Add_StepDependencies(multiengine_maya_build configure", 1)
    assert len(dependency_call) == 2
    inputs = dependency_call[1].split(")", 1)[0]
    assert "multiengine_maya.cmake" in inputs
    assert "multiengine_accuracy.cmake" in inputs


def test_maya_external_configure_inherits_explicit_fast_math_choice():
    source = (ROOT / "engine/cmake/multiengine.cmake").read_text()
    assert "-DMULTIENGINE_FAST_MATH=${MULTIENGINE_FAST_MATH}" in source
