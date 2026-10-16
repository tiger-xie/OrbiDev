include_guard(GLOBAL)
include(FetchContent)

set(RAD_CONPTY_ARCHIVE "" CACHE FILEPATH "离线 ConPTY nupkg 路径；留空则下载固定官方版本")
set(conpty_version "1.25.260930003")
set(conpty_url "https://api.nuget.org/v3-flatcontainer/microsoft.windows.console.conpty/${conpty_version}/microsoft.windows.console.conpty.${conpty_version}.nupkg")
if(RAD_CONPTY_ARCHIVE)
    if(NOT EXISTS "${RAD_CONPTY_ARCHIVE}")
        message(FATAL_ERROR "RAD_CONPTY_ARCHIVE does not exist: ${RAD_CONPTY_ARCHIVE}")
    endif()
    # CMake 对本地归档按扩展名识别，NuGet 包实际为 ZIP；副本仍接受同一哈希校验。
    configure_file("${RAD_CONPTY_ARCHIVE}" "${CMAKE_BINARY_DIR}/conpty-offline.zip" COPYONLY)
    set(conpty_url "${CMAKE_BINARY_DIR}/conpty-offline.zip")
endif()
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(rad_conpty
    URL "${conpty_url}"
    URL_HASH SHA256=02B07B349AF66D801159BDF9E440D4A1CE78BB951F37FC8609731665AFDAE7EE
    TLS_VERIFY TRUE
)
FetchContent_MakeAvailable(rad_conpty)

add_library(RADConPtyRuntime SHARED IMPORTED GLOBAL)
set_target_properties(RADConPtyRuntime PROPERTIES
    IMPORTED_IMPLIB "${rad_conpty_SOURCE_DIR}/runtimes/win-x64/lib/uap10.0/conpty.lib"
    IMPORTED_LOCATION "${rad_conpty_SOURCE_DIR}/runtimes/win-x64/native/conpty.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${rad_conpty_SOURCE_DIR}/inc"
)

function(rad_deploy_conpty target)
    # 遵循官方 NuGet targets 的架构子目录布局；每次构建补齐被移走的运行文件。
    add_custom_target(${target}Runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/x64"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:RADConPtyRuntime>" "$<TARGET_FILE_DIR:${target}>/conpty.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${rad_conpty_SOURCE_DIR}/build/native/runtimes/x64/OpenConsole.exe"
            "$<TARGET_FILE_DIR:${target}>/x64/OpenConsole.exe"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PROJECT_SOURCE_DIR}/third_party/ConPTY-LICENSE.txt"
            "$<TARGET_FILE_DIR:${target}>/ConPTY-LICENSE.txt"
        VERBATIM
    )
    add_dependencies(${target} ${target}Runtime)
endfunction()
