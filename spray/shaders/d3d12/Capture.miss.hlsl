// Capture.miss.hlsl (D3D12)
//
// HLSL equivalent of shaders/vulkan/Capture.rmiss. Not currently wired into
// spray/CMakeLists.txt -- see Capture.raygen.hlsl's header comment.

struct RayPayload
{
    float3 color;
};

[shader("miss")]
void MissMain(inout RayPayload payload)
{
    payload.color = float3(0.05, 0.05, 0.08);
}
