#version 460
#extension GL_EXT_ray_tracing : require

// Capture.rchit
//
// IMPORTANT LIMITATION: PathTracer's bind group layout (see PathTracer.cpp's
// constructor) only exposes a camera UBO, the output image, and the TLAS --
// no per-instance vertex/index/material buffers are bound here yet. That
// means this shader can't compute a real surface normal or sample a
// material, so it can't do lit shading the way Mesh.hlsl's rasterizer does.
// Barycentric-tinted color below is a placeholder good enough to confirm
// rays land on the right geometry; real shading needs vertex/index data
// exposed to this shader (bindless SRVs, or buffer_reference-style access
// keyed by hit-group/instance index) -- that's follow-up work, not done
// here.
//
// Compiled filename is "capture.chit.spv" (see spray/CMakeLists.txt's
// spray_compile_glsl call).

layout(location = 0) rayPayloadInEXT vec3 hitColor;
hitAttributeEXT vec2 attribs;

void main()
{
    vec3 bary = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    hitColor = bary;
}
