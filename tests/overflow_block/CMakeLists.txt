cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(outpost_overflow_block)

zephyr_include_directories(src)
target_include_directories(app PRIVATE ${ZEPHYR_EMBARCH_OUTPOST_MODULE_DIR}/src)
target_sources(app PRIVATE src/main.c)
