add_executable(RemoteAgent main.cpp)
rad_configure_target(RemoteAgent)
target_link_libraries(RemoteAgent PRIVATE RADCore RADProtocol Qt5::Core Qt5::Sql)

if(BUILD_TESTING)
    add_test(NAME agent.smoke COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RemoteAgent> --smoke-test)
    set_tests_properties(agent.smoke PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()
