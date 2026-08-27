# embarch-outpost — Zephyr module.
# ../embarch-doc/embarch-outpost/design.md §5.

if(NOT CONFIG_EMBARCH_OUTPOST)
  return()
endif()

zephyr_include_directories(include)
zephyr_include_directories(${CMAKE_CURRENT_BINARY_DIR}/embarch-outpost)

zephyr_library_named(embarch_outpost)
zephyr_library_sources(
  src/outpost.c
  src/outpost_ring.c
  src/outpost_wire.c
  src/outpost_hooks.c
  src/outpost_markers.c
)


# ---- the application's marker registration list ----------------------------
#
# CONFIG_..._MARKER_HEADER reaches C as a quoted string, which is what #include
# wants — but an unset one reaches it as "", which #include cannot take, and no
# preprocessor test tells the two apart. So the decision is made here, where
# the value is actually inspectable.
set(_outpost_marker_header "${CONFIG_EMBARCH_OUTPOST_MARKER_HEADER}")
string(REPLACE "\"" "" _outpost_marker_header "${_outpost_marker_header}")
if(NOT "${_outpost_marker_header}" STREQUAL "")
  zephyr_compile_definitions(OUTPOST_HAS_MARKER_HEADER=1)
endif()

# ---- build ID --------------------------------------------------------------
#
# decision 9's *verification* half. Compile-time, in a generated header — not
# the post-link CRC patch an earlier draft specified. Three things that must
# agree for a manifest to describe this image: the DUT repo's own describe, the
# outpost module's pinned revision (§5.1), and the marker registration list.
#
# The marker component is a hash of the names as CMake sees them, so a
# reordered or renamed marker changes the build ID even when neither git tree
# moved — which is exactly the case that would otherwise relabel every marker
# in a trace and look entirely readable.

function(outpost_git_describe dir out_var)
  set(_desc "unknown")
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${dir}")
    execute_process(
      COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags
      WORKING_DIRECTORY ${dir}
      OUTPUT_VARIABLE _out
      ERROR_QUIET
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND NOT "${_out}" STREQUAL "")
      set(_desc "${_out}")
    endif()
  endif()
  set(${out_var} "${_desc}" PARENT_SCOPE)
endfunction()

outpost_git_describe("${CMAKE_CURRENT_LIST_DIR}" OUTPOST_MODULE_DESC)
outpost_git_describe("${APPLICATION_SOURCE_DIR}" OUTPOST_APP_DESC)

# Read the marker names straight out of the registration header, the same
# single declaration the enum and the manifest come from.
set(_outpost_marker_names "")
if(NOT "${_outpost_marker_header}" STREQUAL "")
  foreach(_dir ${APPLICATION_SOURCE_DIR} ${APPLICATION_SOURCE_DIR}/src
               ${APPLICATION_SOURCE_DIR}/include ${ZEPHYR_BASE}/include)
    if(EXISTS "${_dir}/${_outpost_marker_header}")
      file(READ "${_dir}/${_outpost_marker_header}" _hdr_text)
      string(REGEX MATCHALL "X\\(([A-Za-z_][A-Za-z0-9_]*)\\)" _matches "${_hdr_text}")
      foreach(_m ${_matches})
        string(REGEX REPLACE "^X\\((.*)\\)$" "\\1" _name "${_m}")
        list(APPEND _outpost_marker_names "${_name}")
      endforeach()
      break()
    endif()
  endforeach()
endif()
string(REPLACE ";" "," _outpost_marker_csv "${_outpost_marker_names}")
string(MD5 _outpost_marker_hash "${_outpost_marker_csv}")
string(SUBSTRING "${_outpost_marker_hash}" 0 8 _outpost_marker_hash)

set(OUTPOST_BUILD_ID "${OUTPOST_APP_DESC}+op${OUTPOST_MODULE_DESC}+m${_outpost_marker_hash}")
string(SUBSTRING "${OUTPOST_BUILD_ID}" 0 ${CONFIG_EMBARCH_OUTPOST_BUILD_ID_MAX} OUTPOST_BUILD_ID)

set(OUTPOST_VERSION "${OUTPOST_MODULE_DESC}")
string(SUBSTRING "${OUTPOST_VERSION}" 0 ${CONFIG_EMBARCH_OUTPOST_BUILD_ID_MAX} OUTPOST_VERSION)

configure_file(
  ${CMAKE_CURRENT_LIST_DIR}/cmake/outpost_build_id.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/embarch-outpost/outpost_build_id.h
  @ONLY)

# ---- post-link manifest generator ------------------------------------------
#
# decision 9's *selection* half's raw material, and decisions 6/7/8's payoff:
# marker IDs, _k_thread_obj_* -> thread names, _sw_isr_table[] index -> handler
# names, all read out of the linked ELF. The engineer never handles this file —
# embarch-api picks it up from the build directory and hands it to Core
# alongside the artifact, because the failure mode of forgetting is not a
# visible error but a silently mislabelled trace.
set(OUTPOST_MANIFEST ${PROJECT_BINARY_DIR}/outpost-manifest.json)

# Deferred to the end of Zephyr's own directory scope, for two reasons found by
# building it rather than by reading CMake:
#
#  * Everything that names the final image — the target that produces it, and
#    BYPRODUCT_KERNEL_*_NAME — is set in zephyr/CMakeLists.txt *after* module
#    CMakeLists run, so none of it exists to be referenced here.
#  * On a native_sim build the two candidate answers differ and only one is
#    right. `zephyr.elf` there is a *relocatable object* whose symbol addresses
#    are all zero; the image is the host executable the native simulator links
#    from it. Zephyr's own `extra_post_build_commands` hook fires on the .elf
#    rule, which is upstream of that link — the wrong hook, and one that fails
#    by producing a manifest full of zeroes rather than by erroring.
# Passed through GLOBAL properties rather than as DEFER CALL arguments: the
# deferred call runs in another directory's scope, and a global property is the
# one channel that is unambiguously the same value on both sides of it.
set_property(GLOBAL PROPERTY embarch_outpost_script
  ${CMAKE_CURRENT_LIST_DIR}/scripts/gen_outpost_manifest.py)
set_property(GLOBAL PROPERTY embarch_outpost_manifest_path ${OUTPOST_MANIFEST})
set_property(GLOBAL PROPERTY embarch_outpost_build_id "${OUTPOST_BUILD_ID}")
set_property(GLOBAL PROPERTY embarch_outpost_version "${OUTPOST_VERSION}")
set_property(GLOBAL PROPERTY embarch_outpost_cycles "${CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC}")

# The record layout version the manifest declares has to be the one the
# firmware puts in its header frame, and it was a literal here and a #define in
# src/outpost_priv.h with nothing holding the two together — a drift that fails
# as every manifest being refused against a firmware that is actually fine.
# Read it out of the header, which is where the format is specified.
file(READ ${CMAKE_CURRENT_LIST_DIR}/src/outpost_priv.h _outpost_priv_h)
string(REGEX MATCH "#define[ 	]+OUTPOST_RECORD_LAYOUT_VERSION[ 	]+([0-9]+)"
       _ "${_outpost_priv_h}")
set(OUTPOST_LAYOUT_VERSION "${CMAKE_MATCH_1}")
if("${OUTPOST_LAYOUT_VERSION}" STREQUAL "")
  message(FATAL_ERROR
    "embarch-outpost: could not read OUTPOST_RECORD_LAYOUT_VERSION out of "
    "src/outpost_priv.h. Guessing it would produce manifests that are refused "
    "against perfectly good firmware.")
endif()
set_property(GLOBAL PROPERTY embarch_outpost_layout "${OUTPOST_LAYOUT_VERSION}")

# GPIO callback records carry a handler *function* pointer, and nothing at link
# time can predict which functions those will be (gpio_init_callback() runs at
# runtime). So the manifest has to carry the whole symbol table — but only for
# an image that emits those records, because it is ~10x the rest of the file.
if(CONFIG_EMBARCH_OUTPOST_TRACE_GPIO)
  set_property(GLOBAL PROPERTY embarch_outpost_functions "--functions")
else()
  set_property(GLOBAL PROPERTY embarch_outpost_functions "")
endif()

function(embarch_outpost_manifest)
  get_property(_script GLOBAL PROPERTY embarch_outpost_script)
  get_property(_out GLOBAL PROPERTY embarch_outpost_manifest_path)
  get_property(_build_id GLOBAL PROPERTY embarch_outpost_build_id)
  get_property(_version GLOBAL PROPERTY embarch_outpost_version)
  get_property(_cycles GLOBAL PROPERTY embarch_outpost_cycles)
  get_property(_layout GLOBAL PROPERTY embarch_outpost_layout)
  get_property(_functions GLOBAL PROPERTY embarch_outpost_functions)

  if(TARGET native_runner_executable)
    set(_target native_runner_executable)
    set(_image ${BYPRODUCT_KERNEL_EXE_NAME})
  else()
    set(_target ${logical_target_for_zephyr_elf})
    set(_image ${BYPRODUCT_KERNEL_ELF_NAME})
  endif()

  add_custom_command(TARGET ${_target} POST_BUILD
    COMMAND ${PYTHON_EXECUTABLE} ${_script}
            --elf ${_image}
            --out ${_out}
            --build-id ${_build_id}
            --outpost-version ${_version}
            --layout-version ${_layout}
            --cycles-per-sec-config ${_cycles}
            ${_functions}
    BYPRODUCTS ${_out}
    VERBATIM)
endfunction()

cmake_language(DEFER DIRECTORY "${ZEPHYR_BASE}" CALL embarch_outpost_manifest)
