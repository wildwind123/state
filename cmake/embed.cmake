# Usage: cmake -DIN=file -DOUT=file.c -DNAME=symbol -P embed.cmake
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR len "${hexlen} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE ${OUT}
    "#include <stddef.h>\n"
    "const unsigned char ${NAME}[] = {${bytes}0x00};\n"
    "const size_t ${NAME}_len = ${len};\n")
