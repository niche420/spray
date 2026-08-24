// mesh.hlsl
//
// Minimal mesh shader for Rasterizer. Register/space convention: binding
// -> register number, bind-group-layout index -> register space, matching
// D3D12GraphicsDevice::BuildRootSignature's comment and mirrored on the
// Vulkan side by descriptor set index == bind group layout index.

cbuffer CameraUniforms : register(b0, space0)
{
    float4x4 gViewProj;
};

cbuffer ObjectUniforms : register(b0, space1)
{
    float4x4 gModel;
};

struct VSInput
{
    float3 position : ATTRIB0;
    float3 normal : ATTRIB1;
    float2 uv : ATTRIB2;
};

struct VSOutput
{
    float4 clipPosition : SV_Position;
    float3 worldNormal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;
    float4 worldPos = mul(gModel, float4(input.position, 1.0));
    o.clipPosition = mul(gViewProj, worldPos);
    // NOTE: uses the model matrix's upper 3x3 directly rather than its
    // inverse-transpose -- correct only under uniform scale. Known
    // simplification (see App/Rasterizer notes); fix by uploading a
    // second, inverse-transpose matrix in ObjectUniforms once non-uniform
    // scale actually shows up in imported content.
    o.worldNormal = mul((float3x3) gModel, input.normal);
    o.uv = input.uv;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    // Placeholder shading until material binding (base color factor /
    // texture) is wired in -- flat headlight lambert so geometry reads as
    // 3D rather than a flat-shaded silhouette.
    float3 n = normalize(input.worldNormal);
    float ndotl = saturate(dot(n, normalize(float3(0.3, 0.7, 0.5))));
    float3 color = float3(0.6, 0.6, 0.65) * (0.15 + 0.85 * ndotl);
    return float4(color, 1.0);
}
