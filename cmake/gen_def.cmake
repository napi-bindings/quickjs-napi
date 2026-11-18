# Writes a module-definition (.def) file that exports the functions of a
# static archive whose names match REGEX. Used for qjsnapi.dll: QuickJS has
# no __declspec(dllexport) annotations, and the linker of zig ignores
# --export-all-symbols.
#
#   cmake -DLIB=libqjs.a -DOUTPUT=out.def -DREGEX=^JS_ [-DSTRIP_UNDERSCORE=ON]
#         -P gen_def.cmake
#
# STRIP_UNDERSCORE: for 32-bit x86, where the symbol names carry a leading
# "_" that the .def file must not.
#
# The names come from the archive's symbol index (its first member, "/"),
# which lists only the external symbols the archive defines: static
# functions and undefined references, which would make the link fail, are
# not in it. Layout of that member (System V / GNU, also written by llvm-ar
# for COFF): a 32-bit big-endian count, the member offsets, then the names,
# NUL-terminated.

foreach(_var LIB OUTPUT REGEX)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "gen_def.cmake: -D${_var}=... is required")
  endif()
endforeach()

file(READ "${LIB}" _magic LIMIT 8)
if(NOT _magic STREQUAL "!<arch>\n")
  message(FATAL_ERROR "${LIB} is not an ar archive")
endif()
# Member header: name[16] date[12] uid[6] gid[6] mode[8] size[10] fmag[2]
file(READ "${LIB}" _header OFFSET 8 LIMIT 60)
string(SUBSTRING "${_header}" 0 16 _name)
string(STRIP "${_name}" _name)
if(NOT _name STREQUAL "/")
  message(FATAL_ERROR "${LIB} has no symbol index (run ranlib on it)")
endif()
string(SUBSTRING "${_header}" 48 10 _size)
string(STRIP "${_size}" _size)

# Read the member as hex: CMake strings cannot hold NUL bytes.
file(READ "${LIB}" _hex OFFSET 68 LIMIT ${_size} HEX)
string(SUBSTRING "${_hex}" 0 8 _count_hex)
math(EXPR _count "0x${_count_hex}")
math(EXPR _names_start "(1 + ${_count}) * 8") # hex digits: count + offsets
string(SUBSTRING "${_hex}" ${_names_start} -1 _names_hex)

# Byte -> character table for the printable ASCII range.
foreach(_code RANGE 33 126)
  math(EXPR _byte "${_code}" OUTPUT_FORMAT HEXADECIMAL)
  string(SUBSTRING "${_byte}" 2 -1 _byte)
  string(LENGTH "${_byte}" _len)
  if(_len EQUAL 1)
    set(_byte "0${_byte}")
  endif()
  string(ASCII ${_code} _char)
  set("_chr_${_byte}" "${_char}")
endforeach()

set(_exports)
set(_current "")
set(_seen 0)
string(REGEX MATCHALL ".." _bytes "${_names_hex}")
foreach(_b IN LISTS _bytes)
  if(_b STREQUAL "00")
    # 32-bit x86 decorates C names with "_"; a .def file lists them without.
    # (Not REGEX REPLACE "^_": it would strip every leading underscore.)
    if(STRIP_UNDERSCORE AND _current MATCHES "^_")
      string(SUBSTRING "${_current}" 1 -1 _current)
    endif()
    if(_current MATCHES "${REGEX}")
      list(APPEND _exports "${_current}")
    endif()
    set(_current "")
    math(EXPR _seen "${_seen} + 1")
    if(_seen EQUAL _count)
      break()
    endif()
  elseif(DEFINED "_chr_${_b}")
    string(APPEND _current "${_chr_${_b}}")
  else()
    message(FATAL_ERROR "unexpected byte 0x${_b} in a symbol name of ${LIB}")
  endif()
endforeach()
list(REMOVE_DUPLICATES _exports)
list(SORT _exports)
list(LENGTH _exports _count)
if(_count EQUAL 0)
  message(FATAL_ERROR "no symbol of ${LIB} matches ${REGEX}")
endif()

list(JOIN _exports "\n  " _body)
file(WRITE "${OUTPUT}.tmp" "EXPORTS\n  ${_body}\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
