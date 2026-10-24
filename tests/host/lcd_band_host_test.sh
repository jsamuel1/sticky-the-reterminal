#!/bin/bash
# lcd_band host test — compiles the SHIPPED firmware/main/lcd_band.h: the row
# alignment a colour panel's partial refresh applies (CO5300: 2-row windows).
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main lcd_band_cases.cpp -o /tmp/lcd_band_test
/tmp/lcd_band_test
echo "lcd_band host test: ALL GREEN"
