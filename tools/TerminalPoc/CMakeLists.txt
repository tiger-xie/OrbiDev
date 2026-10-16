# 所有配置共用同一份生成资源，避免切换 Debug/Release 后嵌入过期脚本。
qt5_add_resources(terminal_resources ${PROJECT_SOURCE_DIR}/frontend/terminal/terminal.qrc)
add_executable(TerminalPoc main.cpp TerminalBridge.h TerminalBridge.cpp ${terminal_resources})
set_target_properties(TerminalPoc PROPERTIES AUTOMOC ON CXX_EXTENSIONS OFF)
target_compile_features(TerminalPoc PRIVATE cxx_std_20)
target_compile_options(TerminalPoc PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
target_link_libraries(TerminalPoc PRIVATE Qt5::Widgets Qt5::WebEngineWidgets Qt5::WebChannel)
if(RAD_ENABLE_ANALYSIS)
    # 保留项目源码分析，仅跳过旧版 Qt 外部头的分析诊断。
    target_compile_options(TerminalPoc PRIVATE /analyze /analyze:external-)
endif()

get_target_property(qt_qmake Qt5::qmake IMPORTED_LOCATION)
get_filename_component(qt_bin "${qt_qmake}" DIRECTORY)
set_target_properties(TerminalPoc PROPERTIES VS_DEBUGGER_ENVIRONMENT "PATH=${qt_bin};%PATH%")
if(BUILD_TESTING)
    add_test(NAME terminal.webengine COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${qt_bin};$ENV{PATH}" $<TARGET_FILE:TerminalPoc> --self-test)
    set_tests_properties(terminal.webengine PROPERTIES TIMEOUT 100 RUN_SERIAL TRUE)
endif()
