cmake_minimum_required(VERSION 3.21)
project(OrbiDevSshWorkbench VERSION 0.1.0 LANGUAGES CXX)
include(CTest)
get_filename_component(ORBIDEV_ROOT "../.." ABSOLUTE)
file(STRINGS "${ORBIDEV_ROOT}/VERSION" ORBIDEV_RELEASE_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ORBIDEV_ROOT}/VERSION")
find_package(Qt5 5.15.2 EXACT REQUIRED COMPONENTS Core Widgets Network WebEngineWidgets WebChannel)
get_target_property(setup_qmake Qt5::qmake IMPORTED_LOCATION)
get_filename_component(setup_qt_bin "${setup_qmake}" DIRECTORY)
add_subdirectory(../SshSetup setup)
include("${ORBIDEV_ROOT}/cmake/ConPtyRuntime.cmake")

add_library(WorkbenchPty STATIC "${ORBIDEV_ROOT}/platforms/windows/ConPty/ConPtyProcess.cpp")
target_include_directories(WorkbenchPty PUBLIC "${ORBIDEV_ROOT}/platforms/windows/ConPty")
target_link_libraries(WorkbenchPty PRIVATE RADConPtyRuntime)
target_compile_definitions(WorkbenchPty PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
setup_target(WorkbenchPty)

add_library(WorkbenchCore STATIC PingService.cpp PingService.h RemoteDirectoryService.cpp RemoteDirectoryService.h WorkbenchStore.cpp WorkbenchStore.h SshSession.cpp SshSession.h ../ProjectLauncher/LauncherCore.cpp)
target_include_directories(WorkbenchCore PUBLIC . ../ProjectLauncher ../SshSetup)
target_link_libraries(WorkbenchCore PUBLIC SshSetupCore Qt5::Network PRIVATE WorkbenchPty iphlpapi ws2_32)
setup_target(WorkbenchCore)
add_library(WorkbenchUi STATIC RemoteDirectoryDialog.cpp RemoteDirectoryDialog.h HostDialog.cpp HostDialog.h WorkbenchWindow.cpp WorkbenchWindow.h
    "${ORBIDEV_ROOT}/apps/RemoteClient/terminal/TerminalView.cpp"
    "${ORBIDEV_ROOT}/apps/RemoteClient/terminal/TerminalBridge.h"
    "${ORBIDEV_ROOT}/frontend/terminal/client-terminal.qrc")
set_target_properties(WorkbenchUi PROPERTIES AUTORCC ON)
target_include_directories(WorkbenchUi PUBLIC . "${ORBIDEV_ROOT}/apps/RemoteClient")
target_link_libraries(WorkbenchUi PUBLIC WorkbenchCore SshSetupUi Qt5::WebEngineWidgets PRIVATE Qt5::WebChannel)
setup_target(WorkbenchUi)
add_executable(OrbiDevWorkbench WIN32 main.cpp)
target_compile_definitions(OrbiDevWorkbench PRIVATE ORBIDEV_VERSION="${ORBIDEV_RELEASE_VERSION}")
target_link_libraries(OrbiDevWorkbench PRIVATE WorkbenchUi)
setup_target(OrbiDevWorkbench)

# 单独部署复用的 ConPTY 运行库，不改变旧工程的部署规则。
add_custom_target(WorkbenchRuntime ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:OrbiDevWorkbench>/x64"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:RADConPtyRuntime>" "$<TARGET_FILE_DIR:OrbiDevWorkbench>/conpty.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${rad_conpty_SOURCE_DIR}/build/native/runtimes/x64/OpenConsole.exe" "$<TARGET_FILE_DIR:OrbiDevWorkbench>/x64/OpenConsole.exe"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ORBIDEV_ROOT}/third_party/ConPTY-LICENSE.txt" "$<TARGET_FILE_DIR:OrbiDevWorkbench>/ConPTY-LICENSE.txt"
    VERBATIM)
add_dependencies(OrbiDevWorkbench WorkbenchRuntime)
if(BUILD_TESTING)
    add_executable(FakeCli "${ORBIDEV_ROOT}/tests/fixtures/FakeCli/main.cpp")
    setup_target(FakeCli)
    target_compile_definitions(FakeCli PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    find_package(Qt5 5.15.2 EXACT REQUIRED COMPONENTS Test)
    foreach(suite IN ITEMS PingService RemoteDirectoryService RemoteDirectoryDialog SshSession WorkbenchWindow ProjectConfiguration WorkbenchStore HostDialog)
        add_executable(${suite}Tests ${suite}Tests.cpp)
        target_link_libraries(${suite}Tests PRIVATE WorkbenchUi Qt5::Test)
        setup_target(${suite}Tests)
        add_dependencies(${suite}Tests WorkbenchRuntime)
        add_test(NAME workbench.${suite} COMMAND ${suite}Tests -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/${suite}-results.txt,txt")
        set_tests_properties(workbench.${suite} PROPERTIES ENVIRONMENT "PATH=${setup_qt_bin}\;$ENV{PATH}" TIMEOUT 120)
    endforeach()
    add_dependencies(SshSessionTests FakeCli)
    set_tests_properties(workbench.SshSession workbench.WorkbenchWindow PROPERTIES RUN_SERIAL TRUE)
    add_executable(WorkbenchTerminalTests "${ORBIDEV_ROOT}/tests/ui/TerminalViewTests.cpp")
    target_link_libraries(WorkbenchTerminalTests PRIVATE WorkbenchUi Qt5::Test)
    setup_target(WorkbenchTerminalTests)
    add_test(NAME workbench.terminal COMMAND WorkbenchTerminalTests -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/terminal-results.txt,txt")
    set_tests_properties(workbench.terminal PROPERTIES ENVIRONMENT "PATH=${setup_qt_bin}\;$ENV{PATH}" TIMEOUT 120 RUN_SERIAL TRUE)
endif()
