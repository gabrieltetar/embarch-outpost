cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(outpost_unit)

zephyr_include_directories(src)
# The wire encoder and the ring are internal to the module, so the tests reach
# for src/ directly rather than through a public header that would exist only
# to be tested through.
target_include_directories(app PRIVATE ${ZEPHYR_EMBARCH_OUTPOST_MODULE_DIR}/src)
target_sources(app PRIVATE src/main.c)
