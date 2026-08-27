#pragma once

#include "Device.hpp"
#include "CommandList.hpp"

#include <cstdint>
#include <vector>

namespace spray::graphics {

// Synchronously reads a texture's pixel data back to CPU memory. Blocks
// via device.WaitForFence until the GPU copy completes -- correct and
// simple for offline dataset capture (the CaptureLayer this exists for),
// not something to call from an interactive per-frame path; each call is
// its own command list submission plus a full fence wait.
//
// `texture` must currently be in ResourceState::ShaderReadOnly -- true of
// any IViewport::GetColorOutput() per its documented contract. This
// function handles the CopySrc transition and back internally.
//
// width/height/bytesPerPixel must match the texture's actual format and
// mip 0 dimensions -- e.g. for PathTracer's Format::RGBA32_Float output,
// bytesPerPixel = 16. No format introspection happens here; get it wrong
// and you get garbage or an out-of-bounds copy, not an error.
std::vector<uint8_t> ReadbackTexture(IDevice& device, TextureHandle texture,
                                      uint32_t width, uint32_t height, uint32_t bytesPerPixel);

} // namespace spray::graphics