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
