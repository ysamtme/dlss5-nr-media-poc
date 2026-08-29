cmake_minimum_required(VERSION 3.24)
project(dlss5_nr_media_poc VERSION 0.0.1 LANGUAGES CXX)

set(NGX_SDK_DIR "" CACHE PATH "Path to an official NVIDIA NGX/DLSS SDK containing include/")
option(DLSSNR_D3D_DEBUG "Enable the D3D12 debug layer and DRED in release-runtime probes" ON)
if(NOT EXISTS "${NGX_SDK_DIR}/include/nvsdk_ngx.h")
    message(FATAL_ERROR "Set -DNGX_SDK_DIR=<official NVIDIA DLSS SDK root>")
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(dlssnr-image src/main.cpp)
add_library(ngx-caller SHARED src/ngx_caller.cpp)
add_library(ngx-core STATIC src/ngx_core.cpp)
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
if(NOT EXISTS "${IMGUI_DIR}/imgui.cpp")
    message(FATAL_ERROR
        "Dear ImGui submodule is missing. Run: git submodule update --init --depth 1")
endif()
add_library(imgui STATIC
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp")
target_include_directories(imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_compile_definitions(imgui PRIVATE UNICODE _UNICODE NOMINMAX)

add_executable(dlssnr-gui WIN32 src/gui.cpp src/gui_processor.cpp)
add_executable(dlssnr-gui-smoke src/gui_processor_smoke.cpp src/gui_processor.cpp)

function(add_version_resource target description original_filename file_type)
    set(VERSION_FILE_DESCRIPTION "${description}")
    set(VERSION_INTERNAL_NAME "${target}")
    set(VERSION_ORIGINAL_FILENAME "${original_filename}")
    set(VERSION_FILE_TYPE "${file_type}")
    set(resource "${CMAKE_CURRENT_BINARY_DIR}/${target}-version.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/version.rc.in" "${resource}" @ONLY)
    target_sources(${target} PRIVATE "${resource}")
endfunction()

add_version_resource(dlssnr-image "dlss5-nr-media-poc image processor"
    "dlssnr-image.exe" VFT_APP)
add_version_resource(dlssnr-gui "dlss5-nr-media-poc graphical interface"
    "dlssnr-gui.exe" VFT_APP)
add_version_resource(ngx-caller "dlss5-nr-media-poc interoperability caller shim"
    "nvngx.dll" VFT_DLL)

target_compile_features(dlssnr-gui PRIVATE cxx_std_20)
target_compile_features(dlssnr-gui-smoke PRIVATE cxx_std_20)
target_compile_definitions(dlssnr-gui PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_definitions(dlssnr-gui-smoke PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(dlssnr-gui PRIVATE "${NGX_SDK_DIR}/include")
target_include_directories(dlssnr-gui-smoke PRIVATE "${NGX_SDK_DIR}/include")
target_link_libraries(dlssnr-gui PRIVATE imgui ngx-core
    d3d12 d3dcompiler dxgi dxguid ole32 shell32 windowscodecs)
target_link_libraries(dlssnr-gui-smoke PRIVATE ngx-core
    d3d12 dxgi dxguid ole32 windowscodecs)
set_target_properties(ngx-caller PROPERTIES OUTPUT_NAME nvngx PREFIX "")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(ngx-caller PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_CURRENT_BINARY_DIR}/${config}/caller")
endforeach()

target_compile_features(dlssnr-image PRIVATE cxx_std_20)
target_compile_features(ngx-caller PRIVATE cxx_std_20)
target_compile_features(ngx-core PRIVATE cxx_std_20)
target_compile_definitions(dlssnr-image PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_definitions(ngx-caller PRIVATE UNICODE _UNICODE NOMINMAX)
target_compile_definitions(ngx-core PRIVATE UNICODE _UNICODE NOMINMAX)
target_include_directories(dlssnr-image PRIVATE "${NGX_SDK_DIR}/include")
target_include_directories(ngx-caller PRIVATE "${NGX_SDK_DIR}/include")
target_include_directories(ngx-core PRIVATE "${NGX_SDK_DIR}/include")
target_link_libraries(ngx-core PUBLIC advapi32)
if(DLSSNR_D3D_DEBUG)
    target_compile_definitions(dlssnr-image PRIVATE DLSSNR_D3D_DEBUG=1)
endif()
target_link_libraries(dlssnr-image PRIVATE
    ngx-core
    d3d12 dxgi dxguid ole32 version windowscodecs)

if(MSVC)
    target_compile_options(dlssnr-image PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(ngx-caller PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(ngx-core PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(dlssnr-gui PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(dlssnr-gui-smoke PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(imgui PRIVATE /W0)
endif()

add_dependencies(dlssnr-image ngx-caller)
add_dependencies(dlssnr-gui dlssnr-image)
add_dependencies(dlssnr-gui-smoke ngx-caller)

install(TARGETS dlssnr-image dlssnr-gui RUNTIME DESTINATION ".")
install(TARGETS ngx-caller RUNTIME DESTINATION "caller")
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md
    PLACE_NVNGX_DLSSNR_DLL_HERE.txt DESTINATION ".")
install(FILES "${IMGUI_DIR}/LICENSE.txt" DESTINATION "licenses/Dear ImGui")
