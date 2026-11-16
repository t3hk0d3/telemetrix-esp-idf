# Fuzz targets, built with clang and libFuzzer outside ESP-IDF: cmake -S test/fuzz -B build -DCMAKE_C_COMPILER=clang
cmake_minimum_required(VERSION 3.16)
project(telemetrix_fuzz C)

set(REPO ${CMAKE_CURRENT_LIST_DIR}/../..)
set(IDF_PATH $ENV{IDF_PATH})
if(NOT IDF_PATH)
    set(IDF_PATH $ENV{HOME}/esp/esp-idf-v6.1)
endif()

# esp_err.h needs esp_compiler.h from the same IDF directory and, through it, an sdkconfig.h: shim/ supplies a minimal one.
set(FUZZ_INCLUDES ${REPO}/include ${REPO}/src/proto ${REPO}/src/core ${REPO}/src/adapter ${REPO}/src/net
    ${REPO}/src/drivers ${REPO}/src/drivers/fake
    ${IDF_PATH}/components/esp_common/include ${CMAKE_CURRENT_LIST_DIR}/shim)
file(GLOB PROTO_SRCS ${REPO}/src/proto/*.c)
file(GLOB CORE_SRCS ${REPO}/src/core/*.c)
file(GLOB ADAPTER_SRCS ${REPO}/src/adapter/*.c)
file(GLOB FAKE_SRCS ${REPO}/src/drivers/fake/*.c)
# The plugin emit ring (its lock is a no-op under TMX_FUZZ) and the test plugin, which emits directly under TMX_FUZZ.
set(PLUGIN_SRCS ${REPO}/src/net/emit_ring.c ${REPO}/test_host/main/test_plugin.c)

function(add_fuzz_target name)
    add_executable(${name} ${name}.c ${ARGN})
    target_include_directories(${name} PRIVATE ${FUZZ_INCLUDES})
    target_compile_definitions(${name} PRIVATE TMX_FUZZ)
    target_compile_options(${name} PRIVATE -g -O1 -Wall -Wextra -fsanitize=fuzzer,address,undefined
                           -fno-sanitize-recover=undefined)
    target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
endfunction()

add_fuzz_target(fuzz_frame ${PROTO_SRCS})
add_fuzz_target(fuzz_adapter ${PROTO_SRCS} ${CORE_SRCS} ${ADAPTER_SRCS} ${FAKE_SRCS} ${PLUGIN_SRCS})
target_link_libraries(fuzz_adapter PRIVATE pthread)
