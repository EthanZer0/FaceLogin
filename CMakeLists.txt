cmake_minimum_required(VERSION 3.20)
project(FaceLogin VERSION 1.6.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FACELOGIN_BUILD_VISION_COMPONENTS
       "Build the face service and enrollment console (requires dlib/ONNX Runtime)"
       ON)

# dlib and ONNX Runtime are only needed by the vision components. Keeping the
# credential provider independently buildable makes native locale changes
# quick to validate and enables a lightweight CI job.
if(FACELOGIN_BUILD_VISION_COMPONENTS)
    find_package(dlib CONFIG REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
endif()

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
add_subdirectory(credential_provider)
if(FACELOGIN_BUILD_VISION_COMPONENTS)
    add_subdirectory(face_service)
    add_subdirectory(enrollment_app)
endif()
