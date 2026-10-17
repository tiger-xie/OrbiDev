add_executable(RADProtocolTests ProtocolTests.cpp)
rad_configure_target(RADProtocolTests)
target_link_libraries(RADProtocolTests PRIVATE RADProtocol Qt5::Test)
add_test(NAME protocol.codec COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RADProtocolTests>)
set_tests_properties(protocol.codec PROPERTIES TIMEOUT 30)
