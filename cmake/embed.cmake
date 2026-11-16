# Embeds a text file into a C header as a NUL-terminated byte array.
#
#   cmake -DINPUT=<file> -DOUTPUT=<header> -DNAME=<symbol> -P embed.cmake
#
# Produces:
#   static const unsigned char <NAME>[] = { ..., 0 };
#   static const size_t <NAME>_len = <size without the NUL>;
#
# A byte array avoids every escaping issue of string literals and the length
# limits some compilers put on them (MSVC: 16 KiB per literal).
foreach(_var INPUT OUTPUT NAME)
  if(NOT DEFINED ${_var})
    message(FATAL_ERROR "embed.cmake: ${_var} is required")
  endif()
endforeach()

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _hex_len)
math(EXPR _size "${_hex_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
# 16 bytes per line keeps the generated file readable.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n  " _bytes
                     "${_bytes}")

file(WRITE "${OUTPUT}"
     "/* Generated from ${INPUT} by cmake/embed.cmake - do not edit. */\n"
     "#include <stddef.h>\n"
     "static const unsigned char ${NAME}[] = {\n  ${_bytes}0x00\n};\n"
     "static const size_t ${NAME}_len = ${_size};\n")
