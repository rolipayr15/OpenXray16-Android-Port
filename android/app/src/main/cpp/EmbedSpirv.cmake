if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED VARIABLE)
    message(FATAL_ERROR "EmbedSpirv.cmake requires INPUT, OUTPUT and VARIABLE")
endif()

file(READ "${INPUT}" spirv HEX)
string(LENGTH "${spirv}" hex_length)
math(EXPR byte_count "${hex_length} / 2")
math(EXPR remainder "${byte_count} % 4")
if(NOT remainder EQUAL 0)
    message(FATAL_ERROR "SPIR-V byte count must be divisible by four: ${INPUT}")
endif()

string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," byte_literals "${spirv}")
file(WRITE "${OUTPUT}"
    "#pragma once\n\nalignas(4) inline constexpr unsigned char ${VARIABLE}[] = {${byte_literals}};\n")
