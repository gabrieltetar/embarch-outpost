# Host-only: `cmake -P tests/build_id_compose.cmake`. With both describes 60
# characters long and BUILD_ID_MAX at its range minimum, two marker lists that
# differ in one name must still produce different build IDs.
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/outpost_compose_build_id.cmake)

string(REPEAT "a" 60 _long)
foreach(_max 16 64 128)
  string(MD5 _h1 "ALPHA,BETA")
  string(MD5 _h2 "ALPHA,BETB")
  string(SUBSTRING "${_h1}" 0 8 _h1)
  string(SUBSTRING "${_h2}" 0 8 _h2)
  outpost_compose_build_id(_id1 "${_long}" "${_long}" "${_h1}" ${_max})
  outpost_compose_build_id(_id2 "${_long}" "${_long}" "${_h2}" ${_max})
  string(LENGTH "${_id1}" _len)
  if(_len GREATER _max)
    message(FATAL_ERROR "max ${_max}: build ID is ${_len} characters")
  endif()
  if("${_id1}" STREQUAL "${_id2}")
    message(FATAL_ERROR "max ${_max}: a renamed marker left the build ID unchanged")
  endif()
endforeach()

# Short describes are not padded or cut.
outpost_compose_build_id(_short "abc1234" "def5678" "01234567" 64)
if(NOT "${_short}" STREQUAL "abc1234+opdef5678+m01234567")
  message(FATAL_ERROR "short describes changed: ${_short}")
endif()
message(STATUS "PASS: build ID keeps the marker hash at every BUILD_ID_MAX tested")
