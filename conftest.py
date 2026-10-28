"""Repository-root pytest configuration.

Routine `pytest -q` collects only tools/secure/ (pytest.ini).  Those tests import upstream's tokenizer through
tools/secure/secure_tokenizer.py, so the upstream/ submodule must be checked out; stop with the instruction
instead of an import traceback when it is not.
"""
from pathlib import Path

import pytest

UPSTREAM_TOKENIZER = Path(__file__).resolve().parent / "upstream" / "tools" / "strata_tokenizer.py"


def pytest_sessionstart(session):
    if not UPSTREAM_TOKENIZER.is_file():
        pytest.exit("upstream/ is not checked out (upstream/tools/strata_tokenizer.py is missing): "
                    "run `git submodule update --init` from the repository root", returncode=4)
