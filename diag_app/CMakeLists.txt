add_executable(FaceLoginDiag WIN32
    main.cpp
    resource.rc
)

target_include_directories(FaceLoginDiag PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/enrollment_app/webview2/include
)

target_link_libraries(FaceLoginDiag PRIVATE
    advapi32
    bcrypt
    comctl32
    comdlg32
    mf
    mfplat
    mfreadwrite
    mfuuid
    ole32
    version
    wevtapi
    ${CMAKE_SOURCE_DIR}/enrollment_app/webview2/lib/WebView2LoaderStatic.lib
)

target_compile_options(FaceLoginDiag PRIVATE /utf-8)

set_target_properties(FaceLoginDiag PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources"
)

target_link_options(FaceLoginDiag PRIVATE
    /DYNAMICBASE
    /NXCOMPAT
    /GUARD:CF
    "/MANIFEST:EMBED"
)

if(TARGET onnxruntime::onnxruntime)
    target_compile_definitions(FaceLoginDiag PRIVATE FACELOGIN_HAS_MODEL_PROBE=1)
    add_executable(FaceLoginModelProbe WIN32 model_probe.cpp)
    target_link_libraries(FaceLoginModelProbe PRIVATE onnxruntime::onnxruntime shell32)
    target_compile_options(FaceLoginModelProbe PRIVATE /utf-8)
    set_target_properties(FaceLoginModelProbe PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/installer/FaceLoginSetup/resources"
    )
    target_link_options(FaceLoginModelProbe PRIVATE
        /DYNAMICBASE
        /NXCOMPAT
        /GUARD:CF
        "/MANIFEST:EMBED"
    )
endif()
