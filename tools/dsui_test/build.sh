#!/bin/sh
# Builds the PC test harness for the dual-screen host core with the LLVM clang that ships with Windows tooling.
cd "$(dirname "$0")"
clang++ -std=c++20 -O2 -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -o dsui_test.exe dsui_test.cpp ../../app/src/main/cpp/dsui/dsui.cpp ../../app/src/main/cpp/dsui/dsraster.cpp 2>&1
