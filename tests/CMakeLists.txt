add_executable(photometric_pipeline_tests
    photometric_pipeline_tests.cpp
)

target_link_libraries(photometric_pipeline_tests PRIVATE
    facelogin_common
    dlib::dlib
)

target_include_directories(photometric_pipeline_tests PRIVATE
    ${CMAKE_SOURCE_DIR}
)

set_target_properties(photometric_pipeline_tests PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

add_test(NAME photometric_pipeline_tests COMMAND photometric_pipeline_tests)
