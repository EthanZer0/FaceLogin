add_executable(FaceLoginService
    main.cpp
    FaceService.cpp
    landmark_detector.cpp
    liveness_detector.cpp
    onnx_models.cpp
    webcam_capture.cpp
    webcam_capture_dshow.cpp
    pipe_server.cpp
    credential_store.cpp
    resource.rc
)

target_link_libraries(FaceLoginService PRIVATE
    facelogin_common
    dlib::dlib
    onnxruntime::onnxruntime
    mfplat
    mf
    mfreadwrite
    mfuuid
    strmiids
    advapi32
    ole32
)

target_include_directories(FaceLoginService PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_target_properties(FaceLoginService PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
)

# No console window for service
# set_target_properties(FaceLoginService PROPERTIES
#     WIN32_EXECUTABLE TRUE
# )
