add_executable(WinConPtyBackendTests WinConPtyBackendTests.cpp)
rad_configure_target(WinConPtyBackendTests)
target_compile_definitions(WinConPtyBackendTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(WinConPtyBackendTests PRIVATE RADWindowsTerminal Qt5::Test)
add_dependencies(WinConPtyBackendTests FakeCli)
rad_deploy_conpty(WinConPtyBackendTests)

foreach(scenario integration argumentsAndTail validation lifecycleStress concurrency backpressure processCleanup)
    add_test(NAME backend.${scenario} COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${RAD_QT_BIN};$ENV{PATH}"
        "RAD_FAKECLI_PATH=$<TARGET_FILE:FakeCli>"
        $<TARGET_FILE:WinConPtyBackendTests> ${scenario}
        -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/m2-${scenario}.xml,junitxml")
    set_tests_properties(backend.${scenario} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endforeach()
set_tests_properties(backend.lifecycleStress PROPERTIES TIMEOUT 180)
