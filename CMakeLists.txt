file(GLOB tmx_proto_srcs "${CMAKE_CURRENT_LIST_DIR}/src/proto/*.c")
file(GLOB tmx_core_srcs "${CMAKE_CURRENT_LIST_DIR}/src/core/*.c")
file(GLOB tmx_adapter_srcs "${CMAKE_CURRENT_LIST_DIR}/src/adapter/*.c")
file(GLOB tmx_net_srcs "${CMAKE_CURRENT_LIST_DIR}/src/net/*.c")

# The linux target runs against the scriptable fake driver, every chip target against the IDF drivers.
if(IDF_TARGET STREQUAL "linux")
    file(GLOB tmx_drv_srcs "${CMAKE_CURRENT_LIST_DIR}/src/drivers/fake/*.c")
else()
    file(GLOB tmx_drv_srcs "${CMAKE_CURRENT_LIST_DIR}/src/drivers/esp/*.c")
endif()

idf_component_register(SRCS ${tmx_proto_srcs} ${tmx_core_srcs} ${tmx_adapter_srcs} ${tmx_drv_srcs} ${tmx_net_srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src/proto" "src/core" "src/adapter" "src/drivers" "src/net"
                       REQUIRES esp_common)
