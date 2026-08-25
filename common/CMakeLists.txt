add_library(facelogin_common STATIC
    ipc_protocol.cpp
    secure_buffer.cpp
    dpapi_util.cpp
    logger.cpp
    config_util.cpp
    account_identity.cpp
    exposure_control.cpp
)

target_include_directories(facelogin_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# dlib is PUBLIC: exposure_control.h uses dlib::matrix in its interface, and
# both face_service and enrollment_app include it through facelogin_common.
target_link_libraries(facelogin_common PUBLIC
    advapi32
    kernel32
    crypt32
    dlib::dlib
)

set_target_properties(facelogin_common PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)
