# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

# Bluetooth features (BT payload / BLE spam) were removed — exclude the NimBLE
# components so they are neither compiled nor linked, freeing flash space.
set(EXCLUDE_COMPONENTS esp-nimble-cpp ESP32-NimBLE-Keyboard)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(projecthydra-32)
