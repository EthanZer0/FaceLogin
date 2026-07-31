add_library(FaceLoginCredentialProvider SHARED
    dllmain.cpp
    FaceLoginProvider.cpp
    FaceLoginCredential.cpp
    pipe_client.cpp
    resource.rc
    credential_provider.def
)

target_link_libraries(FaceLoginCredentialProvider PRIVATE
    facelogin_common
    credui
    advapi32
    ole32
    oleaut32
    kernel32
    shlwapi
    netapi32
    secur32
)

target_include_directories(FaceLoginCredentialProvider PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(FaceLoginCredentialProvider PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# UTF-8 source encoding for Chinese strings
target_compile_options(FaceLoginCredentialProvider PRIVATE /utf-8)

# Security hardening for DLL loaded into LogonUI.exe
target_link_options(FaceLoginCredentialProvider PRIVATE
    /DYNAMICBASE
    /NXCOMPAT
    /GUARD:CF
    /HIGHENTROPYVA
)

# No console
set_target_properties(FaceLoginCredentialProvider PROPERTIES
    WIN32_EXECUTABLE TRUE
)
