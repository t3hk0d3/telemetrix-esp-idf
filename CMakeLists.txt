file(GLOB tmx_proto_srcs "${CMAKE_CURRENT_LIST_DIR}/src/proto/*.c")
file(GLOB tmx_core_srcs "${CMAKE_CURRENT_LIST_DIR}/src/core/*.c")
file(GLOB tmx_adapter_srcs "${CMAKE_CURRENT_LIST_DIR}/src/adapter/*.c")
file(GLOB tmx_net_srcs "${CMAKE_CURRENT_LIST_DIR}/src/net/*.c")

# The network layer: sockets come from lwIP on chips and from the host on linux; the wake is an eventfd (vfs on chips).
if(IDF_TARGET STREQUAL "linux")
    set(tmx_net_requires "")
else()
    set(tmx_net_requires lwip vfs)
endif()

# The linux target runs against the scriptable fake driver, every chip target against the IDF drivers.
if(IDF_TARGET STREQUAL "linux")
    file(GLOB tmx_drv_srcs "${CMAKE_CURRENT_LIST_DIR}/src/drivers/fake/*.c")
else()
    file(GLOB tmx_drv_srcs "${CMAKE_CURRENT_LIST_DIR}/src/drivers/esp/*.c")
endif()

idf_component_register(SRCS ${tmx_proto_srcs} ${tmx_core_srcs} ${tmx_adapter_srcs} ${tmx_drv_srcs} ${tmx_net_srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src/proto" "src/core" "src/adapter" "src/drivers" "src/net"
                       REQUIRES esp_common freertos log ${tmx_net_requires})

# sock.c warns (#warning) when lwIP is built without SO_LINGER; IDF builds components with -Werror, so keep that one a warning.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/src/net/sock.c" PROPERTIES COMPILE_OPTIONS -Wno-error=cpp)
