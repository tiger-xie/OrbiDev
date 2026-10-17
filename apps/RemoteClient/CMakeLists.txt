add_library(RADClientUi STATIC
    controllers/FakeAgent.cpp controllers/FakeAgent.h
    controllers/ClientController.cpp controllers/ClientController.h
    ui/ClientWindow.cpp ui/ClientWindow.h ui/Theme.cpp ui/Theme.h)
rad_configure_target(RADClientUi)
target_include_directories(RADClientUi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RADClientUi PUBLIC Qt5::Widgets)

add_executable(RemoteClient main.cpp WebEngineSmoke.cpp WebEngineSmoke.h)
rad_configure_target(RemoteClient)
target_link_libraries(RemoteClient PRIVATE RADClientUi RADCore RADProtocol Qt5::WebEngineWidgets)

if(BUILD_TESTING)
    add_test(NAME client.smoke COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RemoteClient> --smoke-test)
    set_tests_properties(client.smoke PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
    add_test(NAME client.ui-smoke COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${RAD_QT_BIN};$ENV{PATH}" $<TARGET_FILE:RemoteClient> --ui-smoke-test --demo)
    set_tests_properties(client.ui-smoke PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
endif()
