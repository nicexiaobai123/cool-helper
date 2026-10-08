cmake_minimum_required(VERSION 3.20)
project(DwmHookTests LANGUAGES CXX ASM_MASM)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DwmHookTests requires Windows x64")
endif()
add_library(DwmHookFixture SHARED Win11Fixture.asm)
set_target_properties(DwmHookFixture PROPERTIES
    LINKER_LANGUAGE CXX OUTPUT_NAME dwmcore)
target_link_options(DwmHookFixture PRIVATE /NOENTRY /NODEFAULTLIB /INCREMENTAL:NO
    /EXPORT:FixturePresent /EXPORT:FixtureCall /EXPORT:FixtureThunk /EXPORT:FixtureDuplicate)
add_executable(DwmHookTests
    Win11HookTests.cpp
    InvokeRelay.asm
    ../DwmCfgOverlayLab/AsmHook.asm
    ../DwmCfgOverlayLab/Hooks/HookManager.cpp
    ../DwmCfgOverlayLab/Scanner/PatternScanner.cpp
    ../DwmCfgOverlayLab/Profiles/DwmHookProfiles.cpp
    ../DwmCfgOverlayLab/Platform/SystemProbe.cpp)
target_compile_features(DwmHookTests PRIVATE cxx_std_17)
target_compile_definitions(DwmHookTests PRIVATE UNICODE _UNICODE DWM_OVERLAY_TEST_LOG_STDERR)
target_compile_options(DwmHookTests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
if(MSVC)
    target_compile_options(DwmHookTests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/guard:cf>)
    target_link_options(DwmHookTests PRIVATE /guard:cf /DYNAMICBASE)
endif()
add_dependencies(DwmHookTests DwmHookFixture)
enable_testing()
add_test(NAME Win11LegacyPresentRelay
    COMMAND DwmHookTests "$<TARGET_FILE:DwmHookFixture>")
