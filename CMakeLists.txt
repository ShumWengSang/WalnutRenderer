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
    src/core/CommandLine.cpp
    src/core/CommandLine.h
    src/core/Input.cpp
    src/core/Input.h
    src/core/Log.cpp
    src/core/Log.h
    src/core/SettingsRegistry.cpp
    src/core/SettingsRegistry.h
    src/core/StringUtils.h
    src/core/Timer.h
    src/core/Window.cpp
    src/core/Window.h

    src/gfx/Barriers.h
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
    src/gfx/MipChain.cpp
    src/gfx/MipChain.h
    src/gfx/GpuUtils.h
    src/gfx/PipelineBuilder.cpp
    src/gfx/PipelineBuilder.h
    src/gfx/Pix.cpp
    src/gfx/Pix.h
    src/gfx/RootSignature.cpp
    src/gfx/RootSignature.h
    src/gfx/ShaderCompiler.cpp
    src/gfx/ShaderCompiler.h
    src/gfx/SwapChain.cpp
    src/gfx/SwapChain.h
    src/gfx/Texture.cpp
    src/gfx/Texture.h
    src/gfx/UploadContext.cpp
    src/gfx/UploadContext.h
    src/gfx/Views.h

    src/renderer/AccelerationStructures.cpp
    src/renderer/AccelerationStructures.h
    src/renderer/ClusterGrid.cpp
    src/renderer/ClusterGrid.h
    src/renderer/DebugViews.h
    src/renderer/DeferredPass.cpp
    src/renderer/DeferredPass.h
    src/renderer/FrameConstants.h
    src/renderer/GBuffer.cpp
    src/renderer/GBuffer.h
    src/renderer/LightBuffer.cpp
    src/renderer/LightBuffer.h
    src/renderer/LightMarkerPass.cpp
    src/renderer/LightMarkerPass.h
    src/renderer/PointShadowMaps.cpp
    src/renderer/PointShadowMaps.h
    src/renderer/Renderer.cpp
    src/renderer/Renderer.h
    src/renderer/SceneGpuData.cpp
    src/renderer/SceneGpuData.h
    src/renderer/ScenePass.cpp
    src/renderer/ScenePass.h
    src/renderer/ShadowMap.cpp
    src/renderer/ShadowMap.h
    src/renderer/TonemapPass.cpp
    src/renderer/TonemapPass.h

    src/scene/Assets.cpp
    src/scene/Assets.h
    src/scene/Bounds.h
    src/scene/Camera.cpp
    src/scene/Camera.h
    src/scene/FlyCameraController.cpp
    src/scene/FlyCameraController.h
    src/scene/GltfLoader.cpp
    src/scene/GltfLoader.h
    src/scene/ImageDecoder.cpp
    src/scene/ImageDecoder.h
    src/scene/Lighting.cpp
    src/scene/Lighting.h
    src/scene/PointLights.cpp
    src/scene/PointLights.h
    src/scene/Scene.cpp
    src/scene/Scene.h

    src/ui/ImGuiLayer.cpp
    src/ui/ImGuiLayer.h

    # Shared with the shaders (see shaders/shared/Shared.h).
    shaders/shared/Constants.h
    shaders/shared/DebugViewList.h
    shaders/shared/Shared.h
)

add_executable(WalnutRenderer WIN32 ${WALNUT_SOURCES})
# shaders/: for the headers in shaders/shared/, which the C++ code and the shaders both include.
target_include_directories(WalnutRenderer PRIVATE src shaders)
target_compile_definitions(WalnutRenderer PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
    WALNUT_AGILITY_SDK_VERSION=${WALNUT_AGILITY_SDK_EXPORT_VERSION}
    # Where tools/fetch-assets.ps1 puts models. Absolute, so the exe finds them from any build folder.
    WALNUT_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets"
    # Shaders are compiled from here when it exists, so editing shaders/*.hlsl + F5 reloads them live.
    WALNUT_SHADER_SOURCE_DIR="${PROJECT_SOURCE_DIR}/shaders"
)
target_compile_options(WalnutRenderer PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
target_link_libraries(WalnutRenderer PRIVATE
    imgui
    Microsoft::DirectX-Headers
    walnut_dxc
    walnut_pix
    walnut_cgltf
    walnut_stb
    walnut_stb_write
    d3d12
    dxgi
    dxguid
)
walnut_copy_runtime_dlls(WalnutRenderer)

# Shaders are compiled at runtime from the source tree (WALNUT_SHADER_SOURCE_DIR). The copy next to the exe is the
# fallback for when the exe runs somewhere the source tree doesn't exist.
add_custom_target(WalnutShaders ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
        "${CMAKE_SOURCE_DIR}/shaders" "$<TARGET_FILE_DIR:WalnutRenderer>/shaders"
    COMMENT "Copying shaders"
    VERBATIM
)
add_dependencies(WalnutRenderer WalnutShaders)

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT WalnutRenderer)
