add_executable(FaceLoginConsole
    main.cpp
    WebviewHost.cpp
    EnrollmentWizard.cpp
    resource.rc
    ${CMAKE_SOURCE_DIR}/face_service/webcam_capture.cpp
    ${CMAKE_SOURCE_DIR}/face_service/landmark_detector.cpp
    ${CMAKE_SOURCE_DIR}/face_service/liveness_detector.cpp
    ${CMAKE_SOURCE_DIR}/face_service/onnx_models.cpp
    ${CMAKE_SOURCE_DIR}/face_service/credential_store.cpp
)

target_link_libraries(FaceLoginConsole PRIVATE
    facelogin_common
    dlib::dlib
    onnxruntime::onnxruntime
    mfplat
    mf
    mfreadwrite
    mfuuid
    strmiids
    comctl32
    advapi32
    ole32
    gdi32
    wtsapi32
    crypt32
    windowscodecs
    netapi32
)

target_include_directories(FaceLoginConsole PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/face_service
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2/include
)

# Link WebView2 loader static library
target_link_libraries(FaceLoginConsole PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/webview2/lib/WebView2LoaderStatic.lib
)

# WebView2Loader is statically linked (WebView2LoaderStatic.lib) — no DLL needed

set_target_properties(FaceLoginConsole PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    WIN32_EXECUTABLE TRUE
)

# Custom output directory
set_target_properties(FaceLoginConsole PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources"
)

# Merge the explicit DPI manifest into the linker-generated application
# manifest. This works with both the Visual Studio and Ninja generators.
target_link_options(FaceLoginConsole PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/FaceLoginEnrollment.manifest"
)
