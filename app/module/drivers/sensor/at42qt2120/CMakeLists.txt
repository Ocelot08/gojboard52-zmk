# Copyright (c) 2020 The ZMK Contributors
# SPDX-License-Identifier: MIT

# ZMK's own headers (zmk/activity.h, zmk/event_manager.h, etc.) are only
# added as a PRIVATE include for the main "app" CMake target (app/CMakeLists.txt),
# so an out-of-tree driver module like this one needs to reach for them explicitly.
zephyr_include_directories(../../../../include)
zephyr_include_directories(.)

zephyr_library()

zephyr_library_sources(at42qt2120.c)
