#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
make -C nolibc_httpd
python3 tests/regression.py --variant nolibc-x86_64 "$@"
