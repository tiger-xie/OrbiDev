add_executable(RADClientWindowTests ClientWindowTests.cpp)
rad_configure_target(RADClientWindowTests)
target_link_libraries(RADClientWindowTests PRIVATE RADClientUi Qt5::Test)
add_test(NAME client.window COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RADClientWindowTests>
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/client-window.xml,junitxml")
set_tests_properties(client.window PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
