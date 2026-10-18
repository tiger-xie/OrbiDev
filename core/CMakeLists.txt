add_library(RADCore STATIC model/BuildBaseline.cpp model/BuildBaseline.h)
target_include_directories(RADCore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RADCore PUBLIC Qt5::Core)
rad_configure_target(RADCore)

add_library(RADProtocol STATIC protocol/ProtocolVersion.cpp protocol/ProtocolVersion.h)
target_include_directories(RADProtocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
rad_configure_target(RADProtocol)

target_sources(RADCore PRIVATE
    model/SessionTypes.h model/SessionTypes.cpp
    terminal/ITerminalBackend.h
    provider/IProviderAdapter.h
    session/ISessionManager.h)

target_sources(RADCore PRIVATE session/SessionManager.h session/SessionManager.cpp)

target_sources(RADProtocol PRIVATE protocol/ProtocolCodec.h protocol/ProtocolCodec.cpp)
target_link_libraries(RADProtocol PUBLIC Qt5::Core)

add_library(RADAgentTransport STATIC transport/AgentWebSocketServer.h transport/AgentWebSocketServer.cpp)
target_include_directories(RADAgentTransport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RADAgentTransport PUBLIC RADCore Qt5::Network PRIVATE RADProtocol Qt5::WebSockets)
rad_configure_target(RADAgentTransport)

target_sources(RADCore PRIVATE client/IAgentClient.h)
add_library(RADClientTransport STATIC transport/WebSocketAgentClient.h transport/WebSocketAgentClient.cpp)
target_sources(RADAgentTransport PRIVATE transport/WebSocketReadBuffer.h)
target_sources(RADClientTransport PRIVATE transport/WebSocketReadBuffer.h)
rad_configure_target(RADClientTransport)
target_include_directories(RADClientTransport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(RADClientTransport PUBLIC RADCore PRIVATE RADProtocol Qt5::Network Qt5::WebSockets)

add_library(RADPersistence STATIC persistence/ISessionRepository.h
    persistence/SqliteSessionRepository.h persistence/SqliteSessionRepository.cpp)
target_link_libraries(RADPersistence PUBLIC RADCore Qt5::Sql)
rad_configure_target(RADPersistence)

add_library(RADDiagnostics STATIC diagnostics/DiagnosticLog.h diagnostics/DiagnosticLog.cpp)
target_link_libraries(RADDiagnostics PUBLIC RADCore)
rad_configure_target(RADDiagnostics)
