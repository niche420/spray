#include "pch.hpp"
#include "TextureReadback.hpp"

#include <cstring>

namespace spray::graphics {

std::vector<uint8_t> ReadbackTexture(IDevice& device, TextureHandle texture,
                                      uint32_t width, uint32_t height, uint32_t bytesPerPixel) {
    size_t sizeBytes = static_cast<size_t>(width) * height * bytesPerPixel;

    BufferDesc stagingDesc;
    stagingDesc.sizeBytes = sizeBytes;
    stagingDesc.usage = BufferUsage::CopyDst;
    stagingDesc.hostVisible = true;
    stagingDesc.debugName = "TextureReadback.Staging";
    BufferHandle staging = device.CreateBuffer(stagingDesc);

    ICommandList* cmd = device.BeginCommandList();

    cmd->TransitionTextures({
        { texture, ResourceState::ShaderReadOnly, ResourceState::CopySrc },
    });
    cmd->CopyTextureToBuffer(texture, /*mipLevel=*/0, /*arrayLayer=*/0, staging, /*dstOffset=*/0);
    cmd->TransitionTextures({
        { texture, ResourceState::CopySrc, ResourceState::ShaderReadOnly },
    });

    FenceHandle fence = device.Submit(cmd);
    device.WaitForFence(fence); // staging buffer is HOST_COHERENT -- no explicit flush needed after this

    void* mapped = device.MapBuffer(staging);
    std::vector<uint8_t> out(sizeBytes);
    std::memcpy(out.data(), mapped, sizeBytes);
    device.UnmapBuffer(staging);
    device.DestroyBuffer(staging);

    return out;
}

} // namespace spray::graphics