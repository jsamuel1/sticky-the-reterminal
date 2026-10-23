#pragma once

// One pin map per board; CONFIG_TINY_BOARD_* (Kconfig.projbuild) picks it.
// Every map defines PIN_BTN_OK / PIN_BTN_UP / PIN_BTN_DOWN; a button the board
// does not have is -1, and the shared button/lock code compiles it out.
#include "sdkconfig.h"

#if defined(CONFIG_TINY_BOARD_RETERMINAL)
#include "pin_config_reterminal.h"
#elif defined(CONFIG_TINY_BOARD_FNK0104S)
#include "pin_config_fnk0104s.h"
#elif defined(CONFIG_TINY_BOARD_AMOLED18)
#include "pin_config_amoled18.h"
#else
#error "no CONFIG_TINY_BOARD_* selected (see main/Kconfig.projbuild)"
#endif
