cmake_minimum_required(VERSION 3.20)

project(CoolHelperHub VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "CoolHelperHub currently supports MSVC on Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")

add_library(coolhelper_core STATIC
  src/SseParser.cpp
)
target_include_directories(coolhelper_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
)
target_compile_definitions(coolhelper_core PUBLIC
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
)
target_compile_options(coolhelper_core PRIVATE /W4 /permissive- /utf-8)

add_executable(CoolHelperHub WIN32
  src/main.cpp
  src/App.cpp
  src/AnswerSink.cpp
  src/DllInjector.cpp
  src/Logger.cpp
  src/OpenAIClient.cpp
  src/OverlayControl.cpp
  src/OverlayIpc.cpp
  src/Screenshot.cpp
  src/SettingsStore.cpp
  resources/app.rc
  ${IMGUI_DIR}/imgui.cpp
  ${IMGUI_DIR}/imgui_draw.cpp
  ${IMGUI_DIR}/imgui_tables.cpp
  ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_impl_dx11.cpp
  ${IMGUI_DIR}/imgui_impl_win32.cpp
)
target_include_directories(CoolHelperHub PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources"
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
  "${IMGUI_DIR}"
)
target_compile_definitions(CoolHelperHub PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
)
target_compile_options(CoolHelperHub PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(CoolHelperHub PRIVATE
  coolhelper_core
  d3d11 dxgi dwmapi winhttp windowscodecs crypt32 shell32 ole32 uuid advapi32
)
set_property(TARGET CoolHelperHub PROPERTY VS_DPI_AWARE "PerMonitor")
# Emit a requireAdministrator manifest for the DWM injection workflows.
set_property(TARGET CoolHelperHub PROPERTY
  VS_PROJECT_IMPORT "${CMAKE_CURRENT_SOURCE_DIR}/resources/admin-uac.props")

set_property(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT CoolHelperHub)

include(CTest)
if(BUILD_TESTING)
  add_executable(CoolHelperHubTests tests/SseParserTests.cpp)
  target_link_libraries(CoolHelperHubTests PRIVATE coolhelper_core)
  target_compile_options(CoolHelperHubTests PRIVATE /W4 /permissive- /utf-8)
  add_test(NAME CoolHelperHubTests COMMAND CoolHelperHubTests)

  add_executable(CoolHelperScreenshotTests
    tests/ScreenshotTests.cpp
    src/Screenshot.cpp
  )
  target_include_directories(CoolHelperScreenshotTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
  )
  target_compile_definitions(CoolHelperScreenshotTests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
  )
  target_compile_options(CoolHelperScreenshotTests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(CoolHelperScreenshotTests PRIVATE
    windowscodecs crypt32 ole32 uuid gdi32
  )
  add_test(NAME CoolHelperScreenshotTests COMMAND CoolHelperScreenshotTests)

  add_executable(CoolHelperSettingsTests
    tests/SettingsStoreTests.cpp
    src/SettingsStore.cpp
    src/Logger.cpp
  )
  target_include_directories(CoolHelperSettingsTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
  )
  target_compile_definitions(CoolHelperSettingsTests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
  )
  target_compile_options(CoolHelperSettingsTests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(CoolHelperSettingsTests PRIVATE crypt32 shell32 ole32)
  add_test(NAME CoolHelperSettingsTests COMMAND CoolHelperSettingsTests)

  add_executable(CoolHelperOpenAITests
    tests/OpenAIClientTests.cpp
    src/OpenAIClient.cpp
    src/Screenshot.cpp
    src/Logger.cpp
  )
  target_include_directories(CoolHelperOpenAITests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
  )
  target_compile_definitions(CoolHelperOpenAITests PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
  )
  target_compile_options(CoolHelperOpenAITests PRIVATE /W4 /permissive- /utf-8)
  target_link_libraries(CoolHelperOpenAITests PRIVATE
    coolhelper_core winhttp windowscodecs crypt32 shell32 ole32 uuid ws2_32
  )
  add_test(NAME CoolHelperOpenAITests COMMAND CoolHelperOpenAITests)
endif()

install(TARGETS CoolHelperHub RUNTIME DESTINATION .)
