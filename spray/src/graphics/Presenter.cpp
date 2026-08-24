#include "pch.hpp"
#include "Presenter.hpp"

namespace spray::graphics {

Presenter::Presenter(IDevice& device, shaders::ShaderLibrary& shaderLibrary, Format targetColorFormat)
    : m_device(device), m_shaders(shaderLibrary) {

    m_shaders.Load("blit.vs", m_device);
    m_shaders.Load("blit.ps", m_device);

    // set/binding come from Blit.hlsl's register(t0, space0) / register(s1,
    // space0) via reflection -- same ShaderLibrary-derived pattern as
    // Rasterizer/PathTracer's bind group layouts.
    m_layout = m_device.CreateBindGroupLayout(m_shaders.DeriveBindGroupLayout({ "blit.vs", "blit.ps" }, 0));

    SamplerDesc samplerDesc;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.addressModeU = AddressMode::ClampToEdge;
    samplerDesc.addressModeV = AddressMode::ClampToEdge;
    m_sampler = m_device.CreateSampler(samplerDesc);

    const auto& vs = m_shaders.Load("blit.vs", m_device);
    const auto& ps = m_shaders.Load("blit.ps", m_device);

    GraphicsPipelineDesc desc;
    desc.vertexShader = vs.handle;
    desc.pixelShader = ps.handle;
    desc.bindGroupLayouts = { m_layout };
    // No vertex buffers -- Blit.hlsl's VSMain generates a fullscreen
    // triangle procedurally from SV_VertexID, no buffer-bound geometry
    // needed for a full-screen pass.
    desc.colorTargetFormats = { targetColorFormat };
    desc.depthTargetFormat = Format::Unknown; // no depth test/write for a 2D blit
    desc.depthStencil.depthTestEnable = false;
    desc.depthStencil.depthWriteEnable = false;
    desc.rasterizer.cullMode = CullMode::None; // fullscreen triangle -- no meaningful winding to cull

    m_pipeline = m_device.CreateGraphicsPipeline(desc);
}

Presenter::~Presenter() {
    if (m_bindGroup.IsValid()) m_device.DestroyBindGroup(m_bindGroup);
    m_device.DestroySampler(m_sampler);
    m_device.DestroyPipeline(m_pipeline);
    m_device.DestroyBindGroupLayout(m_layout);
    // Shader modules: owned by ShaderLibrary (shared), not destroyed here
    // -- same convention as Rasterizer/PathTracer.
}

void Presenter::Blit(ICommandList& cmd, TextureHandle source, uint32_t destWidth, uint32_t destHeight) {
    if (!m_bindGroup.IsValid() || source != m_boundTexture) {
        if (m_bindGroup.IsValid()) m_device.DestroyBindGroup(m_bindGroup);

        BindGroupDesc bgDesc;
        bgDesc.layout = m_layout;
        bgDesc.entries = {
            BindGroupEntry::Texture(0, source),
            BindGroupEntry::Sampler(1, m_sampler),
        };
        m_bindGroup = m_device.CreateBindGroup(bgDesc);
        m_boundTexture = source;
    }

    cmd.SetPipeline(m_pipeline);
    cmd.SetBindGroup(0, m_bindGroup);
    // See Rasterizer::Render's comment on why this is needed -- both
    // backends declare viewport/scissor as dynamic pipeline state.
    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(destWidth), static_cast<float>(destHeight));
    cmd.SetScissor(0, 0, destWidth, destHeight);
    cmd.Draw(3, 1, 0, 0);
}

} // namespace spray::graphics