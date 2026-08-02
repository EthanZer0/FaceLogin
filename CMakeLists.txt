cmake_minimum_required(VERSION 3.20)
project(FaceLogin VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find dlib (used by face_service and enrollment_app, NOT by credential_provider)
find_package(dlib CONFIG REQUIRED)
find_package(onnxruntime CONFIG REQUIRED)

# Global compile definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

# Global compiler options
if(MSVC)
    add_compile_options(/W4 /WX- /MP)
    # Force MSVC to parse all source files as UTF-8 (the sources contain raw
    # non-ASCII characters like — and →). Without this, MSVC decodes them
    # using the system code page (e.g. GBK/936), producing garbled wide-char
    # literals that show up as 鈥?/鈫? in the UTF-16 log files.
    add_compile_options(/utf-8)
    # Security hardening
    add_link_options(/DYNAMICBASE /NXCOMPAT /GUARD:CF)
endif()

add_subdirectory(common)
add_subdirectory(face_service)
add_subdirectory(credential_provider)
add_subdirectory(enrollment_app)
