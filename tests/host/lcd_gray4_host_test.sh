#!/bin/bash
# lcd_gray4 host test — compiles the SHIPPED firmware/main/lcd_gray4.h with the
# shipped canvas.cpp/font.cpp: gray4 -> big-endian RGB565 strips and the
# dirty-row band that the colour-LCD partial refresh pushes.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I shim -I ../../firmware/main \
    ../../firmware/main/canvas.cpp ../../firmware/main/font.cpp \
    lcd_gray4_cases.cpp -o /tmp/lcd_gray4_test
/tmp/lcd_gray4_test
echo "lcd_gray4 host test: ALL GREEN"
