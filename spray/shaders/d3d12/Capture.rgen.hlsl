// Capture.raygen.hlsl
//
// Ray generation shader for PathTracer's dataset-capture pass (see
// PathTracer.hpp's class comment -- this traces primary rays only, no
// bounce/GI yet, one sample per pixel). Register/space convention matches
// Mesh.hlsl: binding -> register number, bind-group-layout index -> register
// space. PathTracer only ever binds one bind group (m_sceneLayout), so
// everything here is space0.

cbuffer CameraUniforms : register(b0, space0)
{
    float4x4 gInvView; // camera's world transform (see PathTracer::Render's comment: camWorld already IS this)
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

    // Pixel center -> NDC in [-1, 1], Y flipped to match this engine's
    // raster convention (see VulkanCommandList::SetViewport's comment on
    // why -- keeps a capture's framing consistent with the viewport's).
    float2 ndc = (float2(pixel) + 0.5) / float2(dims) * 2.0 - 1.0;
    ndc.y = -ndc.y;

    // Unproject a point on the far plane in view space, then rotate into
    // world space with the camera's world (== inverse-view) matrix. Origin
    // is just the camera's world-space position (inverse-view applied to
    // the origin).
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