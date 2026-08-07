set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../external/imgui)

set(SRC
    src/App.cpp
    src/App.hpp
    src/Components.hpp
    src/Main.cpp
    src/Scene.hpp
    src/Window.hpp
    src/Window.cpp
    src/graphics/CommandList.hpp
    src/graphics/Context.cpp
    src/graphics/Context.hpp
    src/graphics/Device.hpp
    src/graphics/Types.hpp
    src/ui/UI.cpp
    src/ui/UI.hpp
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
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
        ${IMGUI_DIR}/backends/imgui_impl_vulkan.h)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/entt/single_entt
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