cmake_minimum_required(VERSION 3.16)

# The adapter built with every optional feature switched off (sdkconfig.defaults); run by tools/run_host_tests.sh after
# the main host test project.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_telemetrix_features_off)
