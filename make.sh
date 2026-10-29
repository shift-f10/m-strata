#!/bin/sh
# make.sh - build every binary and engine into one runtime-only folder (make.py; Linux).
#   ./make.sh --out /opt/secure-strata [--ggml-dir ~/llama.cpp] [--cuda-arch auto] [--without maya]
# See `./make.sh --help`. Runs make.py with the repository's dev Python when there is one, else python3.
here=$(cd "$(dirname "$0")" && pwd)
if [ -x "$here/.venv-dev/bin/python" ]; then
  exec "$here/.venv-dev/bin/python" "$here/make.py" "$@"
fi
exec python3 "$here/make.py" "$@"
