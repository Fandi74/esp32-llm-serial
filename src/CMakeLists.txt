# Main component CMakeLists.txt

set(srcs
    "main.c"
)

set(include_dirs
    "."
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/lib/wifi_manager"
    "${CMAKE_SOURCE_DIR}/lib/llm_client"
)

idf_component_register(SRCS ${srcs}
                     INCLUDE_DIRS ${include_dirs}
                     REQUIRES wifi_manager llm_client)
