# Compose the build ID so the marker-list hash survives truncation.
#
# `<app describe>+op<module describe>+m<hash>` cut to BUILD_ID_MAX from the right
# lost the hash first, and the hash is the only part that changes when a marker
# is renamed in a dirty tree. So the tail `+m<hash>` is kept whole and the
# describes are cut instead. BUILD_ID_MAX's range minimum (16) leaves room for
# it: the tail is 10 characters.
function(outpost_compose_build_id out app_desc module_desc marker_hash max_len)
  set(_tail "+m${marker_hash}")
  string(LENGTH "${_tail}" _tail_len)
  if(max_len LESS _tail_len)
    message(FATAL_ERROR "CONFIG_EMBARCH_OUTPOST_BUILD_ID_MAX=${max_len} cannot hold "
                        "the marker-list hash (${_tail_len} characters)")
  endif()
  math(EXPR _head_max "${max_len} - ${_tail_len}")
  set(_head "${app_desc}+op${module_desc}")
  string(LENGTH "${_head}" _head_len)
  if(_head_len GREATER _head_max)
    string(SUBSTRING "${_head}" 0 ${_head_max} _head)
  endif()
  set(${out} "${_head}${_tail}" PARENT_SCOPE)
endfunction()
