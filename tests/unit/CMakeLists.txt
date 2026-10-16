add_executable(RADTests BuildBaselineTests.cpp)
rad_configure_target(RADTests)
target_link_libraries(RADTests PRIVATE RADCore RADProtocol Qt5::Test)
add_test(NAME skeleton.baseline COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RADTests>)
set_tests_properties(skeleton.baseline PROPERTIES TIMEOUT 30)
