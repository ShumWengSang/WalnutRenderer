# Third-party dependencies, all pinned and fetched at configure time.
include(FetchContent)

set(WALNUT_AGILITY_SDK_VERSION "1.620.1")
set(WALNUT_DXC_VERSION "1.9.2609.5")
set(WALNUT_IMGUI_TAG "v1.93.0-docking")

# The Agility SDK "minor" number is what the exe exports as D3D12SDKVersion (e.g. 1.620.1 -> 620).
string(REGEX MATCH "^1\\.([0-9]+)" _ "${WALNUT_AGILITY_SDK_VERSION}")
set(WALNUT_AGILITY_SDK_EXPORT_VERSION "${CMAKE_MATCH_1}")

# Headers matching the Agility SDK runtime (d3d12.h, d3dx12.h, ...).
set(DXHEADERS_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(DXHEADERS_BUILD_GOOGLE_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(directx_headers
    GIT_REPOSITORY https://github.com/microsoft/DirectX-Headers.git
    GIT_TAG        v${WALNUT_AGILITY_SDK_VERSION}
    GIT_SHALLOW    TRUE
)

# Agility SDK runtime (D3D12Core.dll / d3d12SDKLayers.dll) from NuGet.
FetchContent_Declare(agility_sdk
    URL           https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${WALNUT_AGILITY_SDK_VERSION}
    URL_HASH      SHA256=A8A494C3E5F687D9BE0A465EC8248DA70781AB689D888DB8806B600422722BE6
    DOWNLOAD_NAME agility_sdk.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# DirectX Shader Compiler (runtime HLSL -> DXIL) from NuGet.
FetchContent_Declare(dxc
    URL           https://www.nuget.org/api/v2/package/Microsoft.Direct3D.DXC/${WALNUT_DXC_VERSION}
    URL_HASH      SHA256=73CFA082245918C2CC1EC6740250EB7377E83A757B3B3A3E16E0D71535BFBFC3
    DOWNLOAD_NAME dxc.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        ${WALNUT_IMGUI_TAG}
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(directx_headers agility_sdk dxc imgui)

# --- Agility SDK -------------------------------------------------------------
set(WALNUT_AGILITY_BIN_DIR "${agility_sdk_SOURCE_DIR}/build/native/bin/x64")

# --- DXC ---------------------------------------------------------------------
set(WALNUT_DXC_BIN_DIR "${dxc_SOURCE_DIR}/build/native/bin/x64")
add_library(walnut_dxc INTERFACE)
target_include_directories(walnut_dxc INTERFACE "${dxc_SOURCE_DIR}/build/native/include")
target_link_libraries(walnut_dxc INTERFACE "${dxc_SOURCE_DIR}/build/native/lib/x64/dxcompiler.lib")

# --- Dear ImGui (Win32 + DX12 backends) --------------------------------------
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
# Build the DX12 backend against the same d3d12.h as the engine.
target_include_directories(imgui BEFORE PRIVATE "${directx_headers_SOURCE_DIR}/include/directx")
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(imgui PUBLIC d3d12 dxgi PRIVATE d3dcompiler dwmapi)

# Copies the runtime DLLs (Agility SDK + DXC) next to an executable target.
function(walnut_copy_runtime_dlls target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WALNUT_AGILITY_BIN_DIR}/D3D12Core.dll"
            "${WALNUT_AGILITY_BIN_DIR}/D3D12Core.pdb"
            "${WALNUT_AGILITY_BIN_DIR}/d3d12SDKLayers.dll"
            "${WALNUT_AGILITY_BIN_DIR}/d3d12SDKLayers.pdb"
            "$<TARGET_FILE_DIR:${target}>/D3D12/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WALNUT_DXC_BIN_DIR}/dxcompiler.dll"
            "${WALNUT_DXC_BIN_DIR}/dxil.dll"
            "$<TARGET_FILE_DIR:${target}>/"
        COMMENT "Copying Agility SDK and DXC runtime DLLs"
        VERBATIM
    )
endfunction()
