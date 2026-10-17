add_executable(RADTests BuildBaselineTests.cpp)
rad_configure_target(RADTests)
target_link_libraries(RADTests PRIVATE RADCore RADProtocol Qt5::Test)
add_test(NAME skeleton.baseline COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RADTests>)
set_tests_properties(skeleton.baseline PROPERTIES TIMEOUT 30)

add_executable(RADSessionContractTests SessionContractTests.cpp)
rad_configure_target(RADSessionContractTests)
target_link_libraries(RADSessionContractTests PRIVATE RADCore Qt5::Test)
add_test(NAME core.session_contracts COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RADSessionContractTests>)
set_tests_properties(core.session_contracts PROPERTIES TIMEOUT 30)

add_executable(RADSessionManagerTests SessionManagerTests.cpp)
rad_configure_target(RADSessionManagerTests)
target_link_libraries(RADSessionManagerTests PRIVATE RADCore Qt5::Test)
add_test(NAME core.session_manager COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RADSessionManagerTests>
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/session-manager.xml,junitxml")
set_tests_properties(core.session_manager PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
