set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../external/imgui)

set(SRC
    src/App.cpp
    src/App.hpp
    src/Main.cpp
    src/Swapchain.cpp
    src/Swapchain.hpp
    src/Window.hpp
    src/Window.cpp
    src/event/Events.hpp
    src/event/Input.cpp
    src/event/Input.hpp
    src/event/Layer.hpp
    src/event/LayerStack.cpp
    src/event/LayerStack.hpp
    src/asset/AssetManager.cpp
    src/asset/AssetManager.hpp
    src/asset/AssetTypes.hpp
    src/asset/GltfImporter.cpp
    src/asset/GltfImporter.hpp
    src/graphics/CommandList.hpp
    src/graphics/Context.cpp
    src/graphics/Context.hpp
    src/graphics/Device.hpp
    src/graphics/GraphicsTypes.hpp
    src/graphics/HandlePool.hpp
    src/graphics/Pathtracer.cpp
    src/graphics/Pathtracer.hpp
    src/math/Geometry.hpp
    src/scene/Components.hpp
    src/scene/Scene.cpp
    src/scene/Scene.hpp
    src/scene/SceneLayer.cpp
    src/scene/SceneLayer.hpp
    src/scene/SceneRenderer.cpp
    src/scene/SceneRenderer.hpp
    src/ui/UIManager.cpp
    src/ui/UIManager.hpp
    src/ui/UIBackend.hpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
)

# Add platform/backend-specific files
if(SPRAY_ENABLE_VULKAN)
    list(APPEND SRC
        src/graphics/vulkan/VulkanCommandList.cpp
        src/graphics/vulkan/VulkanCommandList.hpp
        src/graphics/vulkan/VulkanCommon.hpp
        src/graphics/vulkan/VulkanContext.cpp
        src/graphics/vulkan/VulkanContext.hpp
        src/graphics/vulkan/VulkanDevice.cpp
        src/graphics/vulkan/VulkanDevice.hpp
        src/ui/VulkanUIBackend.cpp
        src/ui/VulkanUIBackend.hpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    )
endif()
if(SPRAY_ENABLE_D3D12)
    list(APPEND SRC
        src/graphics/d3d12/D3D12CommandList.cpp
        src/graphics/d3d12/D3D12CommandList.hpp
        src/graphics/d3d12/D3D12Common.hpp
        src/graphics/d3d12/D3D12Context.cpp
        src/graphics/d3d12/D3D12Context.hpp
        src/graphics/d3d12/D3D12Device.cpp
        src/graphics/d3d12/D3D12Device.hpp
        ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp
        ${IMGUI_DIR}/backends/imgui_impl_dx12.h)
endif()

add_executable(spray ${SRC})
add_executable(spray::spray ALIAS spray)

target_link_libraries(spray PRIVATE SDL3::SDL3 Vulkan::Vulkan)

target_include_directories(spray PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/entt/single_include
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/tinygltf
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/stb
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_precompile_headers(spray
    PRIVATE
        src/pch.hpp
)

if(SPRAY_ENABLE_VULKAN)
    target_compile_definitions(spray PRIVATE SPRAY_VULKAN_ENABLED)
endif()
if(SPRAY_ENABLE_D3D12)
    target_compile_definitions(spray PRIVATE SPRAY_D3D12_ENABLED)
endif()

get_target_property(SPRAY_SOURCES spray SOURCES)
source_group(
    TREE "${CMAKE_SOURCE_DIR}"
    PREFIX ""
    FILES ${SPRAY_SOURCES}
)


set(SPRAY_SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders/compiled)
file(MAKE_DIRECTORY ${SPRAY_SHADER_OUT_DIR})
set(SPRAY_SHADER_OUTPUTS "")
set(SPRAY_SHADER_SOURCES "")

find_program(DXC_EXECUTABLE
    NAMES dxc dxc.exe
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin32"
)
if(NOT DXC_EXECUTABLE)
    message(WARNING "spray: dxc not found -- .hlsl shaders won't be compiled.")
endif()

find_program(SLANGC_EXECUTABLE
    NAMES slangc slangc.exe
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" "$ENV{SLANG_SDK}/bin"
)
if(NOT SLANGC_EXECUTABLE)
    message(WARNING "spray: slangc not found -- .slang shaders won't be compiled. "
                     "Not used by anything yet, so this is expected for now.")
endif()

function(spray_compile_hlsl SOURCE ENTRY PROFILE STEM)
    if(NOT DXC_EXECUTABLE)
        return()
    endif()
    list(APPEND SPRAY_SHADER_SOURCES ${SOURCE})
    set(SPRAY_SHADER_SOURCES ${SPRAY_SHADER_SOURCES} PARENT_SCOPE)

    if(SPRAY_ENABLE_VULKAN)
        set(SPV_OUT ${SPRAY_SHADER_OUT_DIR}/${STEM}.spv)
        add_custom_command(
            OUTPUT ${SPV_OUT}
            COMMAND ${DXC_EXECUTABLE} -spirv -fspv-target-env=vulkan1.2
                    -T ${PROFILE} -E ${ENTRY} -Fo ${SPV_OUT} ${SOURCE}
            DEPENDS ${SOURCE}
            COMMENT "dxc (SPIR-V): ${STEM}"
            VERBATIM
        )
        list(APPEND SPRAY_SHADER_OUTPUTS ${SPV_OUT})
    endif()

    if(SPRAY_ENABLE_D3D12)
        set(DXIL_OUT ${SPRAY_SHADER_OUT_DIR}/${STEM}.dxil)
        add_custom_command(
            OUTPUT ${DXIL_OUT}
            COMMAND ${DXC_EXECUTABLE} -T ${PROFILE} -E ${ENTRY} -Fo ${DXIL_OUT} ${SOURCE}
            DEPENDS ${SOURCE}
            COMMENT "dxc (DXIL): ${STEM}"
            VERBATIM
        )
        list(APPEND SPRAY_SHADER_OUTPUTS ${DXIL_OUT})
    endif()

    set(SPRAY_SHADER_OUTPUTS ${SPRAY_SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()

function(spray_compile_glsl SOURCE STEM)
    if(NOT SPRAY_ENABLE_VULKAN)
        return()
    endif()
    list(APPEND SPRAY_SHADER_SOURCES ${SOURCE})
    set(SPRAY_SHADER_SOURCES ${SPRAY_SHADER_SOURCES} PARENT_SCOPE)

    set(SPV_OUT ${SPRAY_SHADER_OUT_DIR}/${STEM}.spv)
    add_custom_command(
        OUTPUT ${SPV_OUT}
        COMMAND Vulkan::glslc --target-env=vulkan1.2 -O -o ${SPV_OUT} ${SOURCE}
        DEPENDS ${SOURCE}
        COMMENT "glslc: ${STEM}"
        VERBATIM
    )
    list(APPEND SPRAY_SHADER_OUTPUTS ${SPV_OUT})
    set(SPRAY_SHADER_OUTPUTS ${SPRAY_SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()

function(spray_compile_slang SOURCE ENTRY STEM)
    if(NOT SLANGC_EXECUTABLE)
        return()
    endif()
    list(APPEND SPRAY_SHADER_SOURCES ${SOURCE})
    set(SPRAY_SHADER_SOURCES ${SPRAY_SHADER_SOURCES} PARENT_SCOPE)

    if(SPRAY_ENABLE_VULKAN)
        set(SPV_OUT ${SPRAY_SHADER_OUT_DIR}/${STEM}.spv)
        add_custom_command(
            OUTPUT ${SPV_OUT}
            COMMAND ${SLANGC_EXECUTABLE} ${SOURCE} -entry ${ENTRY} -target spirv -o ${SPV_OUT}
            DEPENDS ${SOURCE}
            COMMENT "slangc (SPIR-V): ${STEM}"
            VERBATIM
        )
        list(APPEND SPRAY_SHADER_OUTPUTS ${SPV_OUT})
    endif()

    if(SPRAY_ENABLE_D3D12)
        set(DXIL_OUT ${SPRAY_SHADER_OUT_DIR}/${STEM}.dxil)
        add_custom_command(
            OUTPUT ${DXIL_OUT}
            COMMAND ${SLANGC_EXECUTABLE} ${SOURCE} -entry ${ENTRY} -target dxil -o ${DXIL_OUT}
            DEPENDS ${SOURCE}
            COMMENT "slangc (DXIL): ${STEM}"
            VERBATIM
        )
        list(APPEND SPRAY_SHADER_OUTPUTS ${DXIL_OUT})
    endif()

    set(SPRAY_SHADER_OUTPUTS ${SPRAY_SHADER_OUTPUTS} PARENT_SCOPE)
endfunction()


spray_compile_hlsl(${CMAKE_CURRENT_SOURCE_DIR}/shaders/d3d12/Mesh.hlsl VSMain vs_6_0 mesh.vs)
spray_compile_hlsl(${CMAKE_CURRENT_SOURCE_DIR}/shaders/d3d12/Mesh.hlsl PSMain ps_6_0 mesh.ps)

spray_compile_glsl(${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/Capture.rgen  capture.raygen)
spray_compile_glsl(${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/Capture.rmiss capture.miss)
spray_compile_glsl(${CMAKE_CURRENT_SOURCE_DIR}/shaders/vulkan/Capture.rchit capture.chit)

add_custom_target(spray_shaders DEPENDS ${SPRAY_SHADER_OUTPUTS} SOURCES ${SPRAY_SHADER_SOURCES})
add_dependencies(spray spray_shaders)

add_custom_command(TARGET spray POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${SPRAY_SHADER_OUT_DIR} $<TARGET_FILE_DIR:spray>/shaders/compiled
)

add_custom_command(TARGET spray POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${PROJECT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:spray>/assets
)