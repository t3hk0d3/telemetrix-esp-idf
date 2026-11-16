cmake_minimum_required(VERSION 3.16)

# The component runs on the IDF linux target against the scriptable fake driver; pytest drives it over TCP.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(telemetrix_host_server)
