add_executable(RADClientWindowTests ClientWindowTests.cpp)
rad_configure_target(RADClientWindowTests)
target_link_libraries(RADClientWindowTests PRIVATE RADClientUi Qt5::Test)
add_test(NAME client.window COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RADClientWindowTests>
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/client-window.xml,junitxml")
set_tests_properties(client.window PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

add_executable(RADTerminalViewTests TerminalViewTests.cpp)
rad_configure_target(RADTerminalViewTests)
target_link_libraries(RADTerminalViewTests PRIVATE RADTerminalView Qt5::Test)
add_test(NAME client.terminal COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RADTerminalViewTests>
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/terminal-view.xml,junitxml")
set_tests_properties(client.terminal PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
