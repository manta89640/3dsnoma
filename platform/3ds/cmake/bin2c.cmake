# cmake -DIN=file.bin -DOUT=file.c -DNAME=symbol -P bin2c.cmake: embed a file as a C array
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" n)
math(EXPR size "${n} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT} "#include <stddef.h>\n#include <stdint.h>\n"
  "__attribute__((aligned(4))) const uint8_t ${NAME}[] = {${bytes}};\n"
  "const size_t ${NAME}_size = ${size};\n")
