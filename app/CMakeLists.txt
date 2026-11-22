# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(wired_switchboard LANGUAGES C)

target_include_directories(app PRIVATE src)

target_sources(app PRIVATE
	src/main.c
	src/device_model.c
	src/io_board.c
	src/app_settings.c
	src/net_t1s.c
	src/status_led.c
)

target_sources_ifdef(CONFIG_APP_MQTT app PRIVATE src/mqtt_app.c)
target_sources_ifdef(CONFIG_APP_SHELL app PRIVATE src/app_shell.c)
