# Per-board HAL. CONFIG_TINY_BOARD_* (Kconfig.projbuild) picks the set; the
# shared tiny_* modules are identical on every board. During IDF's early
# expansion pass CONFIG_* is not defined yet, which is fine: only REQUIRES is
# read in that pass, and REQUIRES is the same for every board (the vendored
# e-ink/touch/gauge libraries are linked only when referenced).
set(TINY_SHARED_SRCS
  "tiny_main.cpp" "tiny_lock.cpp" "board_caps.cpp"
  "canvas.cpp" "font.cpp"
  "tiny_display.cpp" "tiny_touch.cpp" "tiny_config.cpp" "tiny_wifi.cpp" "tiny_node.cpp" "tiny_provision.cpp" "tiny_shell.cpp" "tiny_ble.cpp" "tiny_beacon.cpp"
  "tiny_audio.cpp" "tiny_upload.cpp" "tiny_ota.cpp" "tiny_button.cpp" "tiny_screenshot.cpp"
  "tiny_sensors.cpp"
  "tiny_time.cpp" "tiny_orient.cpp" "tiny_stream.cpp" "tiny_askq.cpp" "tiny_bootmark.cpp" "tiny_agent.cpp" "tiny_onboard.cpp" "tiny_pair.cpp"
  # further tiny/* modules register here as milestones land (M3+)
)

if(CONFIG_TINY_BOARD_FNK0104S)
  # FNK0104S: ST7796S LCD behind the shared colour-panel canvas (lcd_canvas).
  set(TINY_BOARD_SRCS
    "board_generic.cpp" "sticky_power_generic.cpp" "lcd_canvas.cpp" "lcd_panel_st7796.cpp"
    "hal_absent.cpp" "sticky_battery_adc.cpp" "tiny_batt_curve.cpp" "sticky_sdcard_sdmmc.cpp"
    "audio_es8311.cpp" "sticky_buzzer_es8311.cpp" "tiny_pcm.cpp"
    # WS2812 status light (empty unless CONFIG_TINY_STATUS_LED).
    "tiny_led.cpp")
elseif(CONFIG_TINY_BOARD_AMOLED18)
  # AMOLED-1.8 V2: CO5300 QSPI AMOLED behind the shared colour-panel canvas.
  set(TINY_BOARD_SRCS
    "board_generic.cpp" "sticky_power_generic.cpp" "lcd_canvas.cpp" "lcd_panel_co5300.cpp"
    "hal_absent.cpp" "sticky_imu_qmi8658.cpp"
    "audio_es8311.cpp" "sticky_buzzer_es8311.cpp" "tiny_pcm.cpp")
else()
  # reTerminal Sticky (default): Seeed's HAL adapted from the dashboard demo.
  set(TINY_BOARD_SRCS
    "board.cpp" "sticky_display.cpp" "sticky_power.cpp" "sticky_buzzer.cpp" "sticky_sdcard.cpp"
    "sticky_battery.cpp" "sticky_sht40.cpp" "sticky_imu.cpp" "sticky_rtc.cpp")
endif()

idf_component_register(
  SRCS ${TINY_SHARED_SRCS} ${TINY_BOARD_SRCS}
  INCLUDE_DIRS "." "tiny"
  REQUIRES bt esp_wifi esp_http_server mbedtls esp_http_client esp_https_ota nvs_flash app_update json esp_netif lwip
           seeed_epaper gt911 bq27220 button driver esp_driver_ledc esp_driver_uart esp_driver_i2s vfs
           fatfs sdmmc esp_driver_sdspi esp_driver_usb_serial_jtag esp_psram spi_flash esp_adc
           esp_lcd
)

# TINY_FW_COMMIT: build-injected git sha — retires the
# version-collision class: "which commit is on the glass" becomes a read).
set(TINY_COMMIT_H ${CMAKE_CURRENT_BINARY_DIR}/gen/tiny_commit.h)
add_custom_target(tiny_gen_commit ALL
  COMMAND ${CMAKE_COMMAND}
          -DREPO_DIR=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${TINY_COMMIT_H}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_commit.cmake
  BYPRODUCTS ${TINY_COMMIT_H}
  COMMENT "gen tiny_commit.h (git describe --always --dirty)")
add_dependencies(${COMPONENT_LIB} tiny_gen_commit)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)

# Board stamp (board_caps.cpp, .rodata_custom_desc at image offset 288): force
# the symbol into the link so --gc-sections can never drop the one thing an
# OTA image uses to say which board it is for.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u tiny_board_stamp")
