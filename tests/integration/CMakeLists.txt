add_executable(WinConPtyBackendTests WinConPtyBackendTests.cpp)
rad_configure_target(WinConPtyBackendTests)
target_compile_definitions(WinConPtyBackendTests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(WinConPtyBackendTests PRIVATE RADWindowsTerminal Qt5::Test)
add_dependencies(WinConPtyBackendTests FakeCli)
rad_deploy_conpty(WinConPtyBackendTests)

foreach(scenario integration interactiveInput argumentsAndTail validation lifecycleStress concurrency backpressure processCleanup)
    add_test(NAME backend.${scenario} COMMAND ${CMAKE_COMMAND} -E env
        "PATH=${RAD_QT_BIN};$ENV{PATH}"
        "RAD_FAKECLI_PATH=$<TARGET_FILE:FakeCli>"
        $<TARGET_FILE:WinConPtyBackendTests> ${scenario}
        -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/m2-${scenario}.xml,junitxml")
    set_tests_properties(backend.${scenario} PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
endforeach()
set_tests_properties(backend.lifecycleStress PROPERTIES TIMEOUT 180)

add_executable(SessionManagerIntegrationTests SessionManagerIntegrationTests.cpp)
rad_configure_target(SessionManagerIntegrationTests)
target_link_libraries(SessionManagerIntegrationTests PRIVATE RADWindowsTerminal Qt5::Test)
add_dependencies(SessionManagerIntegrationTests FakeCli WinConPtyBackendTestsRuntime)
# 与后端测试共用输出目录和运行库部署目标，避免并行复制同一文件。
add_test(NAME session.integration COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}"
    "RAD_FAKECLI_PATH=$<TARGET_FILE:FakeCli>"
    $<TARGET_FILE:SessionManagerIntegrationTests>
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/session-manager-integration.xml,junitxml")
set_tests_properties(session.integration PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

add_executable(AgentWebSocketServerTests AgentWebSocketServerTests.cpp)
rad_configure_target(AgentWebSocketServerTests)
target_link_libraries(AgentWebSocketServerTests PRIVATE RADAgentTransport RADProtocol RADWindowsTerminal Qt5::WebSockets Qt5::Test)
add_dependencies(AgentWebSocketServerTests FakeCli WinConPtyBackendTestsRuntime)
# 共用集成测试目录下的 ConPTY 运行库，避免重复部署。
add_test(NAME agent.websocket COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}"
    "RAD_FAKECLI_PATH=$<TARGET_FILE:FakeCli>"
    $<TARGET_FILE:AgentWebSocketServerTests>
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/agent-websocket.xml,junitxml")
set_tests_properties(agent.websocket PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

add_executable(RADWebSocketAgentClientTests WebSocketAgentClientTests.cpp)
rad_configure_target(RADWebSocketAgentClientTests)
target_link_libraries(RADWebSocketAgentClientTests PRIVATE RADClientTransport RADAgentTransport RADWindowsTerminal Qt5::WebSockets Qt5::Test)
add_dependencies(RADWebSocketAgentClientTests FakeCli WinConPtyBackendTestsRuntime)
add_test(NAME client.websocket COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" "RAD_FAKECLI_PATH=$<TARGET_FILE:FakeCli>"
    $<TARGET_FILE:RADWebSocketAgentClientTests>
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/client-websocket.xml,junitxml")
set_tests_properties(client.websocket PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

add_executable(RADRemoteTerminalTests RemoteTerminalTests.cpp)
rad_configure_target(RADRemoteTerminalTests)
target_link_libraries(RADRemoteTerminalTests PRIVATE RADClientUi RADClientTransport RADAgentTransport RADWindowsTerminal Qt5::Test)
add_dependencies(RADRemoteTerminalTests FakeCli WinConPtyBackendTestsRuntime)
add_test(NAME client.remote-terminal COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" "RAD_FAKECLI_PATH=$<TARGET_FILE:FakeCli>"
    "RAD_CLIENT_SCREENSHOT=${PROJECT_BINARY_DIR}/m4-02-remote-$<CONFIG>.png"
    $<TARGET_FILE:RADRemoteTerminalTests> realWindowTerminalRoundTrip
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/remote-terminal.xml,junitxml")
set_tests_properties(client.remote-terminal PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

add_test(NAME client.reconnect COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" "RAD_FAKECLI_PATH=$<TARGET_FILE:FakeCli>"
    "RAD_CLIENT_SCREENSHOT=${PROJECT_BINARY_DIR}/m5-01-reconnect-$<CONFIG>.png"
    $<TARGET_FILE:RADRemoteTerminalTests> reconnectAfterOneMinute
    -o "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/reconnect.xml,junitxml")
set_tests_properties(client.reconnect PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)

add_executable(RADAgentPersistenceTests AgentPersistenceTests.cpp)
rad_configure_target(RADAgentPersistenceTests)
target_link_libraries(RADAgentPersistenceTests PRIVATE RADClientTransport Qt5::Network Qt5::Test)
add_dependencies(RADAgentPersistenceTests RemoteAgent FakeCli)
add_test(NAME agent.persistence COMMAND ${CMAKE_COMMAND} -E env
    "PATH=${RAD_QT_BIN};$ENV{PATH}" "RAD_FAKECLI_PATH=$<TARGET_FILE:FakeCli>"
    "RAD_AGENT_PATH=$<TARGET_FILE:RemoteAgent>"
    $<TARGET_FILE:RADAgentPersistenceTests>)
set_tests_properties(agent.persistence PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
