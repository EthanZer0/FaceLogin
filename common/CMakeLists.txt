add_library(facelogin_common STATIC
    ipc_protocol.cpp
    secure_buffer.cpp
    dpapi_util.cpp
    logger.cpp
    config_util.cpp
)

target_include_directories(facelogin_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(facelogin_common PUBLIC
    advapi32
    kernel32
    crypt32
)

set_target_properties(facelogin_common PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
