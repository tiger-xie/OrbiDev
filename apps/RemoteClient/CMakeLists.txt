add_executable(RemoteClient main.cpp)
rad_configure_target(RemoteClient)
target_link_libraries(RemoteClient PRIVATE RADCore RADProtocol Qt5::Widgets Qt5::WebEngineWidgets)

if(BUILD_TESTING)
    add_test(NAME client.smoke COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RemoteClient> --smoke-test)
    set_tests_properties(client.smoke PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
endif()
