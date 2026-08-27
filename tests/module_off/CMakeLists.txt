# Compile-only: does <embarch/outpost.h> still work in an image that does NOT
# have the outpost module compiled in?
#
# That is the shipping build of every application that instruments a hot path,
# and until 2026-08-27 the answer was no: `zephyr_include_directories(include)`
# sat below the CONFIG_EMBARCH_OUTPOST early return, so the header did not
# resolve at all and every OUTPOST_EVT() call site had to be wrapped in the
# application's own #ifdef. Nothing failed loudly — an app simply could not
# put a marker in shared library code.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(outpost_module_off LANGUAGES C)
target_sources(app PRIVATE src/main.c)
