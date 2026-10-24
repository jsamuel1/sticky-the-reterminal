// sdkconfig.h stand-in for the render harness. The harness plays an FNK0104S
// build for the status light only: tiny/tiny_led.h then declares the real
// tiny_led_* entry points, render_cases.cpp records them, and the test can
// assert the alert layer follows each committed card. Nothing else in
// tiny_display.cpp reads CONFIG_*.
#pragma once
#define CONFIG_TINY_STATUS_LED 1
