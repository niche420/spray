// Capture.raygen.hlsl (D3D12)
//
// HLSL equivalent of shaders/vulkan/Capture.rgen, for the D3D12 backend.
// Not currently wired into spray/CMakeLists.txt's shader list -- D3D12's
// ray tracing pipeline path has its own known gaps (see D3D12Device.hpp's
// namespace-mismatch note), so this is kept as source but unbuilt until
// that's sorted out. Register/space convention matches Mesh.hlsl: binding
// -> register number, bind-group-layout index -> register space.

cbuffer CameraUniforms : register(b0, space0)
{
    float4x4 gInvView;
    float4x4 gInvProj;
};

RWTexture2D<float4> gOutput : register(u1, space0);
RaytracingAccelerationStructure gScene : register(t2, space0);

struct RayPayload
{
    float3 color;
};

[shader("raygeneration")]
void RayGenMain()
{
    uint2 pixel = DispatchRaysIndex().xy;
    uint2 dims = DispatchRaysDimensions().xy;

    float2 ndc = (float2(pixel) + 0.5) / float2(dims) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    float4 viewSpaceTarget = mul(gInvProj, float4(ndc, 1.0, 1.0));
    viewSpaceTarget /= viewSpaceTarget.w;

    float3 rayOriginWorld = mul(gInvView, float4(0.0, 0.0, 0.0, 1.0)).xyz;
    float3 rayTargetWorld = mul(gInvView, float4(viewSpaceTarget.xyz, 1.0)).xyz;
    float3 rayDirWorld = normalize(rayTargetWorld - rayOriginWorld);

    RayDesc ray;
    ray.Origin = rayOriginWorld;
    ray.Direction = rayDirWorld;
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    RayPayload payload;
    payload.color = float3(0.0, 0.0, 0.0);

    TraceRay(gScene, RAY_FLAG_NONE, 0xFF, /*hitGroupIndex=*/0, /*multiplier=*/0, /*missIndex=*/0, ray, payload);

    gOutput[pixel] = float4(payload.color, 1.0);
}
