#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
cxx="${CXX:-g++}"
"$cxx" -O3 -std=c++11 -Wall -Wextra -Wpedantic -pthread periodic_read.cpp -o periodic_read
"$cxx" -O3 -std=c++11 -Wall -Wextra -Wpedantic -pthread background_read.cpp -o background_read
