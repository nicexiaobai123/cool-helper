cmake_minimum_required(VERSION 3.20)
project(DwmHookTests LANGUAGES CXX ASM_MASM)
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "DwmHookTests requires Windows x64")
endif()

add_library(DwmHookFixture SHARED Win11Fixture.asm)
set_target_properties(DwmHookFixture PROPERTIES LINKER_LANGUAGE CXX OUTPUT_NAME dwmcore)
target_link_options(DwmHookFixture PRIVATE /NOENTRY /NODEFAULTLIB /INCREMENTAL:NO
    /EXPORT:FixturePresent /EXPORT:FixtureCall /EXPORT:FixtureThunk /EXPORT:FixtureDuplicate)

add_library(DwmDisplayPresentFixture SHARED DisplayPresentFixture.asm)
set_target_properties(DwmDisplayPresentFixture PROPERTIES
    LINKER_LANGUAGE CXX OUTPUT_NAME dwmcore
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/display_present"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/display_present")
target_link_options(DwmDisplayPresentFixture PRIVATE /NOENTRY /NODEFAULTLIB /INCREMENTAL:NO
    /EXPORT:FixturePresent /EXPORT:FixtureCall /EXPORT:FixtureTarget /EXPORT:FixtureDuplicate)

set(hook_sources
    InvokeRelay.asm
    ../DwmCfgOverlayLab/AsmHook.asm
    ../DwmCfgOverlayLab/Hooks/HookManager.cpp
    ../DwmCfgOverlayLab/Scanner/PatternScanner.cpp
    ../DwmCfgOverlayLab/Profiles/DwmHookProfiles.cpp
    ../DwmCfgOverlayLab/Platform/SystemProbe.cpp)
add_executable(DwmHookTests Win11HookTests.cpp ${hook_sources})
add_executable(DwmDisplayPresentTests Win11DDisplayHookTests.cpp ${hook_sources})
foreach(target DwmHookTests DwmDisplayPresentTests)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE DWM_OVERLAY_TEST_LOG_STDERR)
    target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/guard:cf>)
        target_link_options(${target} PRIVATE /guard:cf /DYNAMICBASE)
    endif()
endforeach()
add_dependencies(DwmHookTests DwmHookFixture)
add_dependencies(DwmDisplayPresentTests DwmDisplayPresentFixture)

enable_testing()
add_test(NAME Win11LegacyPresentRelay COMMAND DwmHookTests "$<TARGET_FILE:DwmHookFixture>")
add_test(NAME Win11DDisplayRenderingRelay COMMAND DwmDisplayPresentTests "$<TARGET_FILE:DwmDisplayPresentFixture>")
# Compile shared renderer/router changes without producing or injecting the DLL.
add_library(DwmRenderCompileCheck OBJECT
    ../DwmCfgOverlayLab/Render/OverlayRenderer.cpp
    ../DwmCfgOverlayLab/Render/FrameRouter.cpp
    ../DwmCfgOverlayLab/Render/FrameTarget.cpp
    ../DwmCfgOverlayLab/Render/InvalidationWorker.cpp
    ../DwmCfgOverlayLab/Render/BackdropCompositor.cpp
    ../DwmCfgOverlayLab/Render/DDisplaySurfaceAdapter.cpp
    ../DwmCfgOverlayLab/Render/DisplayTopology.cpp
    ../DwmCfgOverlayLab/Render/DxgiSurfaceAdapter.cpp
    ../DwmCfgOverlayLab/Render/RenderSession.cpp
    ../DwmCfgOverlayLab/Input/InputSource.cpp
    ../DwmCfgOverlayLab/Bootstrap/Runtime.cpp
    ../DwmCfgOverlayLab/IPC/ControlIpc.cpp)
target_compile_features(DwmRenderCompileCheck PRIVATE cxx_std_17)
target_compile_definitions(DwmRenderCompileCheck PRIVATE UNICODE _UNICODE)
target_compile_options(DwmRenderCompileCheck PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)

add_executable(DwmDDisplayAdapterTests DDisplayAdapterTests.cpp
    ../DwmCfgOverlayLab/Render/FrameTarget.cpp
    ../DwmCfgOverlayLab/Render/DDisplaySurfaceAdapter.cpp
    ../DwmCfgOverlayLab/Render/DisplayTopology.cpp
    ../DwmCfgOverlayLab/Render/DxgiSurfaceAdapter.cpp
    ../DwmCfgOverlayLab/Render/BackdropCompositor.cpp ${hook_sources})
target_compile_features(DwmDDisplayAdapterTests PRIVATE cxx_std_17)
target_compile_definitions(DwmDDisplayAdapterTests PRIVATE UNICODE _UNICODE DWM_OVERLAY_TEST_LOG_STDERR)
target_compile_options(DwmDDisplayAdapterTests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/guard:cf>)
target_link_options(DwmDDisplayAdapterTests PRIVATE /guard:cf /DYNAMICBASE)
target_link_libraries(DwmDDisplayAdapterTests PRIVATE d3d11 dxgi d3dcompiler)
add_test(NAME DDisplayNativeAdapter COMMAND DwmDDisplayAdapterTests)

# Real renderer against synthetic WARP textures and a hidden DXGI fixture;
# no DWM injection or desktop screenshot.
set(imgui_sources
    ../DwmCfgOverlayLab/IMGUI/imgui.cpp
    ../DwmCfgOverlayLab/IMGUI/imgui_draw.cpp
    ../DwmCfgOverlayLab/IMGUI/imgui_tables.cpp
    ../DwmCfgOverlayLab/IMGUI/imgui_widgets.cpp
    ../DwmCfgOverlayLab/IMGUI/imgui_impl_dx11.cpp
    ../DwmCfgOverlayLab/IMGUI/imgui_impl_win32.cpp)
add_executable(DwmRenderSessionTests RenderSessionTests.cpp ${imgui_sources}
    ../DwmCfgOverlayLab/Render/OverlayRenderer.cpp
    ../DwmCfgOverlayLab/Render/InvalidationWorker.cpp
    ../DwmCfgOverlayLab/Render/RenderSession.cpp
    ../DwmCfgOverlayLab/Render/FrameTarget.cpp
    ../DwmCfgOverlayLab/Render/DisplayTopology.cpp
    ../DwmCfgOverlayLab/Render/DxgiSurfaceAdapter.cpp
    ../DwmCfgOverlayLab/Render/BackdropCompositor.cpp
    ../DwmCfgOverlayLab/Input/InputSource.cpp
    ../DwmCfgOverlayLab/IPC/UiState.cpp
    ../DwmCfgOverlayLab/UI/OverlayUi.cpp)
target_compile_features(DwmRenderSessionTests PRIVATE cxx_std_17)
target_compile_definitions(DwmRenderSessionTests PRIVATE UNICODE _UNICODE DWM_OVERLAY_TEST_LOG_STDERR)
target_compile_options(DwmRenderSessionTests PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
target_link_libraries(DwmRenderSessionTests PRIVATE d3d11 dxgi d3dcompiler)
add_test(NAME NormalizedRenderSessions COMMAND DwmRenderSessionTests)
