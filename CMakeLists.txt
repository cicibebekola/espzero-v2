cmake_minimum_required(VERSION 3.16)

# Select which app to build. Override with:
#   idf.py -DACTIVE_APP=bt_music build
set(ACTIVE_APP "launcher" CACHE STRING "App component under apps/ to build")
if(ACTIVE_APP STREQUAL "bt_music")
    set(PROJECT_VER "music-2")
    # Music has its own configuration; the launcher's keyboard/file UI keeps
    # the existing sdkconfig and is not affected by these memory reductions.
    set(SDKCONFIG "${CMAKE_SOURCE_DIR}/sdkconfig.music")
    set(SDKCONFIG_DEFAULTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults;${CMAKE_SOURCE_DIR}/sdkconfig.music.defaults")
    set(COMPONENTS main bt_music)
endif()

set(EXTRA_COMPONENT_DIRS
    ${CMAKE_SOURCE_DIR}/components
    ${CMAKE_SOURCE_DIR}/apps/${ACTIVE_APP}
)

set(PARTITION_TABLE_CSV_PATH "${CMAKE_SOURCE_DIR}/partitions.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp_zero)
