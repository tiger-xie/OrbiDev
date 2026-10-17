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
