cmake_minimum_required(VERSION 3.16.0)

# Project root CMakeLists.txt
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Register project components
set(EXTRA_COMPONENT_DIRS lib/wifi_manager lib/llm_client)

project(esp32-llm-serial)