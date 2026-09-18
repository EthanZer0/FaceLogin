cmake_minimum_required(VERSION 3.20)
project(FaceLogin VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(FACELOGIN_BUILD_VISION_COMPONENTS
       "Build the face service and enrollment console (requires dlib/ONNX Runtime)"
       ON)
if(FACELOGIN_BUILD_VISION_COMPONENTS)
    find_package(dlib CONFIG REQUIRED)

    # The shipped applications operate on camera buffers and ONNX tensors; they do
    # not use dlib's file-image loaders.  The vcpkg dlib target advertises JPEG,
    # PNG, and zlib as unconditional transitive link items, which makes
    # z-applocal copy their DLLs even though the final binaries do not import them.
    # Keep only the numerical BLAS/LAPACK and Windows system dependencies needed by
    # this application in dlib's link interface.
    get_target_property(FACELOGIN_DLIB_LINK_LIBRARIES
        dlib::dlib INTERFACE_LINK_LIBRARIES)
    if(FACELOGIN_DLIB_LINK_LIBRARIES)
        list(FILTER FACELOGIN_DLIB_LINK_LIBRARIES INCLUDE
             REGEX "BLAS::BLAS|LAPACK::LAPACK|ws2_32|winmm")
        set_property(TARGET dlib::dlib PROPERTY
            INTERFACE_LINK_LIBRARIES "${FACELOGIN_DLIB_LINK_LIBRARIES}")
    endif()
    unset(FACELOGIN_DLIB_LINK_LIBRARIES)

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
