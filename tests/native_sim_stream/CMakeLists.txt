cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(outpost_native_sim_stream)

# zephyr_include_directories(), not target_include_directories(app ...): the
# outpost module compiles the marker registration header too, and it is not
# part of the `app` target. Any consuming project needs this line.
zephyr_include_directories(src)

target_sources(app PRIVATE src/main.c)
