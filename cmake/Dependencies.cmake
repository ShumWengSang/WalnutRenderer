# Third-party dependencies, all pinned and fetched at configure time.
include(FetchContent)

set(WALNUT_AGILITY_SDK_VERSION "1.620.1")
set(WALNUT_DXC_VERSION "1.9.2609.5")
set(WALNUT_IMGUI_TAG "v1.93.0-docking")
set(WALNUT_PIX_EVENT_RUNTIME_VERSION "1.0.240308001")
set(WALNUT_CGLTF_TAG "v1.15")
set(WALNUT_STB_COMMIT "d14d46f6b44456eb88beac58cba833a4135d4c89")

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

# PIX markers (PIXBeginEvent/PIXScopedEvent), counters and programmatic capture API (pix3.h).
FetchContent_Declare(winpixeventruntime
    URL           https://www.nuget.org/api/v2/package/WinPixEventRuntime/${WALNUT_PIX_EVENT_RUNTIME_VERSION}
    URL_HASH      SHA256=726ACC93D6968E2146261A1E415521747D50AD69894C2B42B5D0D4C29FD66EC4
    DOWNLOAD_NAME winpixeventruntime.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

# Single-header glTF 2.0 parser.
FetchContent_Declare(cgltf
    GIT_REPOSITORY https://github.com/jkuhlmann/cgltf.git
    GIT_TAG        ${WALNUT_CGLTF_TAG}
    GIT_SHALLOW    TRUE
)

# stb_image (JPEG/PNG decoding). stb has no releases, so a commit is pinned; the archive is hash-checked.
FetchContent_Declare(stb
    URL           https://github.com/nothings/stb/archive/${WALNUT_STB_COMMIT}.zip
    URL_HASH      SHA256=7CF8800E0E9A523B7CA91A52D9CEE5E47386F6C49B9F9917855F3CE847692643
    DOWNLOAD_NAME stb.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(directx_headers agility_sdk dxc imgui winpixeventruntime cgltf stb)

# --- Agility SDK -------------------------------------------------------------
set(WALNUT_AGILITY_BIN_DIR "${agility_sdk_SOURCE_DIR}/build/native/bin/x64")

# --- DXC ---------------------------------------------------------------------
set(WALNUT_DXC_BIN_DIR "${dxc_SOURCE_DIR}/build/native/bin/x64")
add_library(walnut_dxc INTERFACE)
target_include_directories(walnut_dxc INTERFACE "${dxc_SOURCE_DIR}/build/native/include")
target_link_libraries(walnut_dxc INTERFACE "${dxc_SOURCE_DIR}/build/native/lib/x64/dxcompiler.lib")

# --- WinPixEventRuntime ------------------------------------------------------
# USE_PIX is defined for every configuration so Release builds can be profiled too;
# the markers cost almost nothing when no PIX tool is attached.
set(WALNUT_PIX_BIN_DIR "${winpixeventruntime_SOURCE_DIR}/bin/x64")
add_library(walnut_pix INTERFACE)
target_include_directories(walnut_pix INTERFACE "${winpixeventruntime_SOURCE_DIR}/Include")
target_link_libraries(walnut_pix INTERFACE "${WALNUT_PIX_BIN_DIR}/WinPixEventRuntime.lib")
target_compile_definitions(walnut_pix INTERFACE USE_PIX)

# --- cgltf -------------------------------------------------------------------
# Header-only: exactly one translation unit defines CGLTF_IMPLEMENTATION. Doing that in a separate library
# keeps cgltf's own warnings out of the engine's /W4 build.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/cgltf_impl.c" "#define CGLTF_IMPLEMENTATION\n#include <cgltf.h>\n")
add_library(walnut_cgltf STATIC "${CMAKE_CURRENT_BINARY_DIR}/cgltf_impl.c")
target_include_directories(walnut_cgltf SYSTEM PUBLIC "${cgltf_SOURCE_DIR}")
target_compile_definitions(walnut_cgltf PRIVATE _CRT_SECURE_NO_WARNINGS)

# --- stb_image ---------------------------------------------------------------
# Same pattern as cgltf. Only the decoders glTF needs (JPEG, PNG), and no stdio: the engine reads files itself
# (UTF-8 safe) and decodes from memory.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_image_impl.c"
    "#define STB_IMAGE_IMPLEMENTATION\n#define STBI_ONLY_JPEG\n#define STBI_ONLY_PNG\n#include <stb_image.h>\n")
add_library(walnut_stb STATIC "${CMAKE_CURRENT_BINARY_DIR}/stb_image_impl.c")
target_include_directories(walnut_stb SYSTEM PUBLIC "${stb_SOURCE_DIR}")
# STBI_NO_STDIO is public so the header declares the same API everywhere it's included.
target_compile_definitions(walnut_stb PUBLIC STBI_NO_STDIO)

# stb_image_write, for --screenshot. Encodes PNG to memory (STBI_WRITE_NO_STDIO); the engine writes the file.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_image_write_impl.c"
    "#define STB_IMAGE_WRITE_IMPLEMENTATION\n#include <stb_image_write.h>\n")
add_library(walnut_stb_write STATIC "${CMAKE_CURRENT_BINARY_DIR}/stb_image_write_impl.c")
target_include_directories(walnut_stb_write SYSTEM PUBLIC "${stb_SOURCE_DIR}")
target_compile_definitions(walnut_stb_write PUBLIC STBI_WRITE_NO_STDIO)

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

# Copies the runtime DLLs (Agility SDK, DXC, WinPixEventRuntime) next to an executable target.
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
            "${WALNUT_PIX_BIN_DIR}/WinPixEventRuntime.dll"
            "$<TARGET_FILE_DIR:${target}>/"
        COMMENT "Copying Agility SDK, DXC and WinPixEventRuntime DLLs"
        VERBATIM
    )
endfunction()
