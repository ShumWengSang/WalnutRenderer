cmake_minimum_required(VERSION 3.26)
project(WalnutRenderer LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "WalnutRenderer targets Windows x64 (D3D12) only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

include(cmake/Dependencies.cmake)

set(WALNUT_SOURCES
    src/main.cpp

    src/core/Application.cpp
    src/core/Application.h
    src/core/Input.cpp
    src/core/Input.h
    src/core/Log.cpp
    src/core/Log.h
    src/core/StringUtils.h
    src/core/Timer.h
    src/core/Window.cpp
    src/core/Window.h

    src/gfx/Buffer.cpp
    src/gfx/Buffer.h
    src/gfx/CommandQueue.cpp
    src/gfx/CommandQueue.h
    src/gfx/D3D12Common.h
    src/gfx/DescriptorHeap.cpp
    src/gfx/DescriptorHeap.h
    src/gfx/Device.cpp
    src/gfx/Device.h
    src/gfx/FrameAllocator.cpp
    src/gfx/FrameAllocator.h
    src/gfx/GpuUtils.h
    src/gfx/Pix.cpp
    src/gfx/Pix.h
    src/gfx/ShaderCompiler.cpp
    src/gfx/ShaderCompiler.h
    src/gfx/SwapChain.cpp
    src/gfx/SwapChain.h
    src/gfx/UploadContext.cpp
    src/gfx/UploadContext.h

    src/renderer/Renderer.cpp
    src/renderer/Renderer.h
    src/renderer/TrianglePass.cpp
    src/renderer/TrianglePass.h

    src/scene/Camera.cpp
    src/scene/Camera.h

    src/ui/ImGuiLayer.cpp
    src/ui/ImGuiLayer.h
)

add_executable(WalnutRenderer WIN32 ${WALNUT_SOURCES})
target_include_directories(WalnutRenderer PRIVATE src)
target_compile_definitions(WalnutRenderer PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WALNUT_AGILITY_SDK_VERSION=${WALNUT_AGILITY_SDK_EXPORT_VERSION}
)
target_compile_options(WalnutRenderer PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
target_link_libraries(WalnutRenderer PRIVATE
    imgui
    Microsoft::DirectX-Headers
    walnut_dxc
    walnut_pix
    d3d12
    dxgi
    dxguid
)
walnut_copy_runtime_dlls(WalnutRenderer)

# Shaders are compiled at runtime, so ship the HLSL sources next to the exe.
add_custom_target(WalnutShaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:WalnutRenderer>/shaders"
    COMMENT "Copying shaders"
    VERBATIM
)
add_dependencies(WalnutRenderer WalnutShaders)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT WalnutRenderer)
