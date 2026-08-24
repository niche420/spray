// Blit.hlsl
//
// Fullscreen blit -- draws whichever IViewport is currently active's
// offscreen color texture (see graphics/Viewport.hpp) into whatever render
// target is currently bound (the swapchain backbuffer, in
// App::RenderFrame's case). See Presenter.hpp for the class that owns this
// pipeline and issues the draw.
//
// No vertex/index buffer needed: VSMain procedurally generates a
// fullscreen triangle from SV_VertexID -- a standard, buffer-free trick
// (3 vertices whose UVs/clip positions cover the whole screen and then
// some, with the excess simply clipped), avoiding a 4-vertex quad + index
// buffer entirely for what's otherwise the simplest possible draw.
//
// Register/space convention matches Mesh.hlsl: binding -> register number,
// bind-group-layout index -> register space. Presenter only ever binds one
// set, so everything here is space0.

Texture2D gSource : register(t0, space0);
SamplerState gSampler : register(s1, space0);

struct VSOutput
{
    float4 clipPosition : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput o;
    // vertexID 0,1,2 -> uv (0,0), (2,0), (0,2) -- a triangle twice the size
    // of the screen, clipped down to exactly the visible quad.
    float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
    o.uv = uv;
    o.clipPosition = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    // Flip Y to match this engine's raster convention (see
    // VulkanCommandList::SetViewport's comment).
    o.clipPosition.y = -o.clipPosition.y;
    return o;
}

float4 PSMain(VSOutput input) : SV_Target
{
    return gSource.Sample(gSampler, input.uv);
}
