// Capture.chit.hlsl (D3D12)
//
// HLSL equivalent of shaders/vulkan/Capture.rchit -- see that file's header
// comment for the shading limitation (no vertex/normal data bound yet, so
// this is a barycentric placeholder). Not currently wired into
// spray/CMakeLists.txt -- see Capture.raygen.hlsl's header comment.

struct RayPayload
{
    float3 color;
};

[shader("closesthit")]
void ClosestHitMain(inout RayPayload payload, in BuiltInTriangleIntersectionAttributes attribs)
{
    float3 bary = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
                          attribs.barycentrics.x,
                          attribs.barycentrics.y);
    payload.color = bary;
}
