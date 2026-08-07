#include "D3D12Device.h"
#include "D3D12CommandList.h"
#include <algorithm>
#include <cstring>
#include <string>

namespace draw::d3d12_backend {

// ============================================================================
// Construction / teardown
// ============================================================================

D3D12GraphicsDevice::D3D12GraphicsDevice(ComPtr<IDXGIFactory6> factory, ComPtr<IDXGIAdapter1> adapter,
                                          void* windowHandle)
    : m_factory(factory), m_windowHandle(windowHandle) {
    HR_CHECK(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&m_device)));

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 opts5{};
    HR_CHECK(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &opts5, sizeof(opts5)));
    if (opts5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED) {
        throw std::runtime_error("Selected adapter does not support DXR (RaytracingTier == NOT_SUPPORTED)");
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR_CHECK(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)));

    auto createHeap = [&](DescriptorHeap& heap, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32_t capacity,
                           bool shaderVisible) {
        heap.type = type;
        heap.capacity = capacity;
        heap.shaderVisible = shaderVisible;
        heap.descriptorSize = m_device->GetDescriptorHandleIncrementSize(type);
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = capacity;
        desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HR_CHECK(m_device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap.heap)));
    };
    // Fixed capacities sized for a modest app -- see DescriptorHeap's comment
    // about bump allocation; grow these or add a free-list if you exceed them.
    createHeap(m_rtvHeap, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 64, false);
    createHeap(m_dsvHeap, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 32, false);
    createHeap(m_cbvSrvUavHeap, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4096, true);
    // 16 is a hard hardware limit for shader-visible SAMPLER heaps on
    // resource-binding Tier 1 hardware (Tier 2/3 allow up to 2048) -- stay at
    // 16 so this doesn't silently break on older GPUs. Raise it if you know
    // your target hardware supports more and need more than 16 distinct
    // sampler *slots* live at once (many bind groups can still share the
    // same underlying sampler description).
    createHeap(m_samplerHeap, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 16, true);
}

D3D12GraphicsDevice::~D3D12GraphicsDevice() {
    WaitIdle();
}

// ============================================================================
// Buffers
// ============================================================================

NativeBuffer D3D12GraphicsDevice::AllocateBuffer(size_t size, D3D12_HEAP_TYPE heapType,
                                                  D3D12_RESOURCE_STATES initialState,
                                                  D3D12_RESOURCE_FLAGS flags) {
    NativeBuffer buf;
    buf.size = size;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = heapType;

    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width = size;
    resDesc.Height = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = 1;
    resDesc.Format = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags = flags;

    HR_CHECK(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, initialState,
                                                nullptr, IID_PPV_ARGS(&buf.resource)));
    buf.gpuAddress = buf.resource->GetGPUVirtualAddress();
    return buf;
}

BufferHandle D3D12GraphicsDevice::CreateBuffer(const BufferDesc& desc) {
    D3D12_HEAP_TYPE heapType = desc.hostVisible ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (HasFlag(desc.usage, BufferUsage::StorageBuffer)) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // UPLOAD heap resources must be created (and stay) in GENERIC_READ --
    // it's the only state D3D12 permits for that heap type.
    D3D12_RESOURCE_STATES initialState =
        desc.hostVisible ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;

    NativeBuffer buf = AllocateBuffer(desc.sizeBytes, heapType, initialState, flags);
    return m_buffers.Add(buf);
}

void D3D12GraphicsDevice::DestroyBuffer(BufferHandle handle) {
    if (!m_buffers.IsValid(handle)) return;
    NativeBuffer& buf = m_buffers.Get(handle);
    if (buf.mapped) buf.resource->Unmap(0, nullptr);
    m_buffers.Remove(handle);
}

void* D3D12GraphicsDevice::MapBuffer(BufferHandle handle) {
    NativeBuffer& buf = m_buffers.Get(handle);
    if (!buf.mapped) {
        D3D12_RANGE readRange{ 0, 0 }; // we don't read back through this pointer
        HR_CHECK(buf.resource->Map(0, &readRange, &buf.mapped));
    }
    return buf.mapped;
}

void D3D12GraphicsDevice::UnmapBuffer(BufferHandle handle) {
    NativeBuffer& buf = m_buffers.Get(handle);
    if (buf.mapped) {
        buf.resource->Unmap(0, nullptr);
        buf.mapped = nullptr;
    }
}

// ============================================================================
// Textures
// ============================================================================

NativeTexture D3D12GraphicsDevice::AllocateTexture(const TextureDesc& desc) {
    NativeTexture tex;
    tex.format = ToDxgiFormat(desc.format);
    tex.width = desc.width;
    tex.height = desc.height;

    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (HasFlag(desc.usage, TextureUsage::RenderTarget)) flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (HasFlag(desc.usage, TextureUsage::DepthStencil)) flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC resDesc{};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Width = desc.width;
    resDesc.Height = desc.height;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels = desc.mipLevels;
    resDesc.Format = tex.format;
    resDesc.SampleDesc.Count = 1;
    resDesc.Flags = flags;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = tex.format;
    D3D12_CLEAR_VALUE* pClear = nullptr;
    D3D12_RESOURCE_STATES initialState = D3D12_RESOURCE_STATE_COMMON;
    if (HasFlag(desc.usage, TextureUsage::RenderTarget)) {
        pClear = &clearValue;
        initialState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (HasFlag(desc.usage, TextureUsage::DepthStencil)) {
        clearValue.DepthStencil = { 1.0f, 0 };
        pClear = &clearValue;
        initialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    HR_CHECK(m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, initialState,
                                                pClear, IID_PPV_ARGS(&tex.resource)));

    if (HasFlag(desc.usage, TextureUsage::RenderTarget)) {
        uint32_t idx = m_rtvHeap.Allocate(1);
        tex.rtvHandle = m_rtvHeap.CpuHandle(idx);
        m_device->CreateRenderTargetView(tex.resource.Get(), nullptr, tex.rtvHandle);
        tex.hasRtv = true;
    }
    if (HasFlag(desc.usage, TextureUsage::DepthStencil)) {
        uint32_t idx = m_dsvHeap.Allocate(1);
        tex.dsvHandle = m_dsvHeap.CpuHandle(idx);
        m_device->CreateDepthStencilView(tex.resource.Get(), nullptr, tex.dsvHandle);
        tex.hasDsv = true;
    }
    // ShaderResource (SRV) descriptors are created lazily at bind-group
    // creation time instead, since SRVs live in m_cbvSrvUavHeap alongside
    // CBVs/UAVs -- consistent with every bind-group resource coming from
    // that one shader-visible heap.
    return tex;
}

TextureHandle D3D12GraphicsDevice::CreateTexture(const TextureDesc& desc) {
    return m_textures.Add(AllocateTexture(desc));
}

void D3D12GraphicsDevice::DestroyTexture(TextureHandle handle) {
    // RTV/DSV descriptor slots are intentionally leaked (bump allocator, see
    // DescriptorHeap) -- same simplification as the CBV/SRV/UAV heap.
    m_textures.Remove(handle);
}

// ============================================================================
// Samplers
// ============================================================================
// Unlike Vulkan's VkSampler, a D3D12 sampler has no standalone live object --
// it's just a D3D12_SAMPLER_DESC written into a heap slot wherever it's used.
// CreateSampler here only stores the translated desc; the actual
// ID3D12Device::CreateSampler heap-write happens per-slot in CreateBindGroup.

SamplerHandle D3D12GraphicsDevice::CreateSampler(const SamplerDesc& desc) {
    NativeSampler native;
    native.desc.Filter = ToD3D12Filter(desc.magFilter, desc.minFilter);
    native.desc.AddressU = ToD3D12AddressMode(desc.addressModeU);
    native.desc.AddressV = ToD3D12AddressMode(desc.addressModeV);
    native.desc.AddressW = native.desc.AddressU; // no separate W wrap mode exposed yet (2D textures only so far)
    native.desc.MaxLOD = D3D12_FLOAT32_MAX;
    native.desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    return m_samplers.Add(native);
}

void D3D12GraphicsDevice::DestroySampler(SamplerHandle handle) {
    m_samplers.Remove(handle);
}

// ============================================================================
// Shader modules
// ============================================================================
// D3D12 has no persistent shader-module object like VkShaderModule -- the
// bytecode is just stored here and referenced directly by pipeline creation.

ShaderModuleHandle D3D12GraphicsDevice::CreateShaderModule(const ShaderModuleDesc& desc) {
    NativeShaderModule mod;
    mod.bytecode = desc.bytecode.dxil;
    mod.stage = desc.stage;
    mod.entryPoint = desc.entryPoint;
    return m_shaderModules.Add(mod);
}

void D3D12GraphicsDevice::DestroyShaderModule(ShaderModuleHandle handle) {
    m_shaderModules.Remove(handle);
}

// ============================================================================
// Bind groups
// ============================================================================

BindGroupLayoutHandle D3D12GraphicsDevice::CreateBindGroupLayout(const BindGroupLayoutDesc& desc) {
    NativeBindGroupLayout native;
    native.entries = desc.entries;
    return m_bindGroupLayouts.Add(native);
}

void D3D12GraphicsDevice::DestroyBindGroupLayout(BindGroupLayoutHandle handle) {
    m_bindGroupLayouts.Remove(handle);
}

BindGroupHandle D3D12GraphicsDevice::CreateBindGroup(const BindGroupDesc& desc) {
    NativeBindGroupLayout& layout = m_bindGroupLayouts.Get(desc.layout);

    // Split by heap destination -- see NativeBindGroup's comment. Order
    // within each group must match BuildRootSignature's range ordering
    // (both iterate layout.entries in original order, filtering by the same
    // predicate), since OffsetInDescriptorsFromTableStart there was APPEND.
    std::vector<const BindGroupLayoutEntry*> resourceEntries, samplerEntries;
    for (const auto& e : layout.entries) {
        (e.type == BindingType::Sampler ? samplerEntries : resourceEntries).push_back(&e);
    }

    auto findEntry = [&](uint32_t binding) -> const BindGroupEntry* {
        for (const auto& e : desc.entries) {
            if (e.binding == binding) return &e;
        }
        return nullptr;
    };

    NativeBindGroup native;

    if (!resourceEntries.empty()) {
        uint32_t start = m_cbvSrvUavHeap.Allocate(static_cast<uint32_t>(resourceEntries.size()));
        for (uint32_t i = 0; i < resourceEntries.size(); ++i) {
            const BindGroupLayoutEntry& layoutEntry = *resourceEntries[i];
            const BindGroupEntry* entry = findEntry(layoutEntry.binding);
            if (!entry) continue;
            D3D12_CPU_DESCRIPTOR_HANDLE dst = m_cbvSrvUavHeap.CpuHandle(start + i);

            switch (layoutEntry.type) {
                case BindingType::UniformBuffer: {
                    NativeBuffer& buf = m_buffers.Get(entry->buffer);
                    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{};
                    cbv.BufferLocation = buf.gpuAddress;
                    cbv.SizeInBytes = static_cast<uint32_t>((buf.size + 255) & ~size_t(255));
                    m_device->CreateConstantBufferView(&cbv, dst);
                    break;
                }
                case BindingType::StorageBuffer: {
                    NativeBuffer& buf = m_buffers.Get(entry->buffer);
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                    uav.Format = DXGI_FORMAT_R32_TYPELESS;
                    uav.Buffer.NumElements = static_cast<uint32_t>(buf.size / 4);
                    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                    m_device->CreateUnorderedAccessView(buf.resource.Get(), nullptr, &uav, dst);
                    break;
                }
                case BindingType::SampledTexture: {
                    NativeTexture& tex = m_textures.Get(entry->texture);
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.Format = tex.format;
                    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.Texture2D.MipLevels = 1;
                    m_device->CreateShaderResourceView(tex.resource.Get(), &srv, dst);
                    break;
                }
                case BindingType::AccelerationStructure: {
                    NativeAccelStruct& tlas = m_tlases.Get(entry->accelerationStructure);
                    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                    srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
                    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srv.RaytracingAccelerationStructure.Location = tlas.address;
                    m_device->CreateShaderResourceView(nullptr, &srv, dst);
                    break;
                }
                case BindingType::Sampler:
                    break; // handled in the samplerEntries loop below
            }
        }
        native.resourceGpuHandle = m_cbvSrvUavHeap.GpuHandle(start);
        native.hasResourceHandle = true;
    }

    if (!samplerEntries.empty()) {
        uint32_t start = m_samplerHeap.Allocate(static_cast<uint32_t>(samplerEntries.size()));
        for (uint32_t i = 0; i < samplerEntries.size(); ++i) {
            const BindGroupLayoutEntry& layoutEntry = *samplerEntries[i];
            const BindGroupEntry* entry = findEntry(layoutEntry.binding);
            if (!entry) continue;
            NativeSampler& sampler = m_samplers.Get(entry->sampler);
            m_device->CreateSampler(&sampler.desc, m_samplerHeap.CpuHandle(start + i));
        }
        native.samplerGpuHandle = m_samplerHeap.GpuHandle(start);
        native.hasSamplerHandle = true;
    }

    return m_bindGroups.Add(native);
}

void D3D12GraphicsDevice::DestroyBindGroup(BindGroupHandle handle) {
    m_bindGroups.Remove(handle); // heap slot intentionally leaked, see DescriptorHeap
}

// ============================================================================
// Root signature (shared by graphics and ray tracing pipelines)
// ============================================================================
// One root parameter (descriptor table) per bind group layout, in order --
// root parameter index N corresponds to SetBindGroup(N, ...), matching the
// Vulkan backend's "descriptor set index == bind group layout index" scheme.
// Register convention: BindGroupLayoutEntry::binding is used directly as the
// HLSL register number, with register space == the layout's position in the
// bindGroupLayouts list (RegisterSpace = setIndex). Shaders must declare
// e.g. `cbuffer Foo : register(b0, space0)` to match binding=0 in the
// bind group layout passed as bindGroupLayouts[0].

D3D12GraphicsDevice::RootSignatureInfo D3D12GraphicsDevice::BuildRootSignature(
    const std::vector<BindGroupLayoutHandle>& layouts) {
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> allResourceRanges(layouts.size());
    std::vector<std::vector<D3D12_DESCRIPTOR_RANGE1>> allSamplerRanges(layouts.size());
    std::vector<D3D12_ROOT_PARAMETER1> rootParams;

    RootSignatureInfo info;
    info.setResourceParam.assign(layouts.size(), -1);
    info.setSamplerParam.assign(layouts.size(), -1);

    for (uint32_t setIndex = 0; setIndex < layouts.size(); ++setIndex) {
        NativeBindGroupLayout& layout = m_bindGroupLayouts.Get(layouts[setIndex]);
        auto& resRanges = allResourceRanges[setIndex];
        auto& sampRanges = allSamplerRanges[setIndex];

        for (const auto& e : layout.entries) {
            D3D12_DESCRIPTOR_RANGE1 range{};
            range.NumDescriptors = 1;
            range.BaseShaderRegister = e.binding;
            range.RegisterSpace = setIndex;
            range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

            if (e.type == BindingType::Sampler) {
                range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                sampRanges.push_back(range);
                continue;
            }
            switch (e.type) {
                case BindingType::UniformBuffer: range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV; break;
                case BindingType::StorageBuffer: range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; break;
                case BindingType::SampledTexture:
                case BindingType::AccelerationStructure:
                    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
                    break;
                case BindingType::Sampler:
                    break; // unreachable, handled above
            }
            resRanges.push_back(range);
        }

        // Resource table (if any), then sampler table (if any) -- each is
        // its own root parameter since a descriptor table can only draw from
        // one heap type, and D3D12 requires CBV/SRV/UAV and SAMPLER
        // descriptors to live in separate heaps.
        if (!resRanges.empty()) {
            D3D12_ROOT_PARAMETER1 param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            param.DescriptorTable.NumDescriptorRanges = static_cast<uint32_t>(resRanges.size());
            param.DescriptorTable.pDescriptorRanges = resRanges.data();
            info.setResourceParam[setIndex] = static_cast<int32_t>(rootParams.size());
            rootParams.push_back(param);
        }
        if (!sampRanges.empty()) {
            D3D12_ROOT_PARAMETER1 param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            param.DescriptorTable.NumDescriptorRanges = static_cast<uint32_t>(sampRanges.size());
            param.DescriptorTable.pDescriptorRanges = sampRanges.data();
            info.setSamplerParam[setIndex] = static_cast<int32_t>(rootParams.size());
            rootParams.push_back(param);
        }
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = static_cast<uint32_t>(rootParams.size());
    desc.Desc_1_1.pParameters = rootParams.data();
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized, error;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&desc, &serialized, &error);
    if (FAILED(hr)) {
        std::string msg = error ? std::string(static_cast<char*>(error->GetBufferPointer()), error->GetBufferSize())
                                 : "(no error blob)";
        throw std::runtime_error("Root signature serialization failed: " + msg);
    }

    HR_CHECK(m_device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                            IID_PPV_ARGS(&info.rootSignature)));
    return info;
}

// ============================================================================
// Graphics pipeline
// ============================================================================
// No render-pass/framebuffer object needed -- D3D12 always specified render
// targets directly in the PSO (RTVFormats/DSVFormat) and bound them via
// OMSetRenderTargets at draw time, which is exactly what BeginRendering maps
// onto in D3D12CommandList. Unlike Vulkan, there's no separate "dynamic
// rendering" extension to opt into here; this is just how D3D12 always worked.

PipelineHandle D3D12GraphicsDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    NativePipeline native;
    RootSignatureInfo rsInfo = BuildRootSignature(desc.bindGroupLayouts);
    native.rootSignature = rsInfo.rootSignature;
    native.setResourceRootParam = rsInfo.setResourceParam;
    native.setSamplerRootParam = rsInfo.setSamplerParam;
    for (const auto& vb : desc.vertexBuffers) native.vertexStrides.push_back(vb.stride);

    NativeShaderModule& vs = m_shaderModules.Get(desc.vertexShader);
    NativeShaderModule& ps = m_shaderModules.Get(desc.pixelShader);

    // Semantic name is fixed as "ATTRIB" with SemanticIndex == location --
    // app-side HLSL must declare inputs as ATTRIB0, ATTRIB1, ... matching
    // VertexAttribute::location, mirroring how the Vulkan backend expects
    // `layout(location = N)`.
    std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
    for (uint32_t b = 0; b < desc.vertexBuffers.size(); ++b) {
        const auto& vb = desc.vertexBuffers[b];
        for (const auto& attr : vb.attributes) {
            D3D12_INPUT_ELEMENT_DESC elem{};
            elem.SemanticName = "ATTRIB";
            elem.SemanticIndex = attr.location;
            elem.Format = ToDxgiFormat(attr.format);
            elem.InputSlot = b;
            elem.AlignedByteOffset = attr.offset;
            elem.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
            inputElements.push_back(elem);
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = native.rootSignature.Get();
    psoDesc.VS = { vs.bytecode.data(), vs.bytecode.size() };
    psoDesc.PS = { ps.bytecode.data(), ps.bytecode.size() };
    psoDesc.InputLayout = { inputElements.data(), static_cast<uint32_t>(inputElements.size()) };
    psoDesc.PrimitiveTopologyType = desc.topology == PrimitiveTopology::TriangleList
        ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
        : desc.topology == PrimitiveTopology::LineList ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                                         : D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    native.topology = desc.topology == PrimitiveTopology::TriangleList ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                     : desc.topology == PrimitiveTopology::LineList ? D3D_PRIMITIVE_TOPOLOGY_LINELIST
                                                                     : D3D_PRIMITIVE_TOPOLOGY_POINTLIST;

    psoDesc.RasterizerState.FillMode = desc.rasterizer.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = desc.rasterizer.cullMode == CullMode::None ? D3D12_CULL_MODE_NONE
        : desc.rasterizer.cullMode == CullMode::Front ? D3D12_CULL_MODE_FRONT
                                                        : D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE; // matches the Vulkan backend's VK_FRONT_FACE_COUNTER_CLOCKWISE
    psoDesc.RasterizerState.DepthClipEnable = TRUE;

    psoDesc.DepthStencilState.DepthEnable = desc.depthStencil.depthTestEnable;
    psoDesc.DepthStencilState.DepthWriteMask =
        desc.depthStencil.depthWriteEnable ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    switch (desc.depthStencil.depthCompareOp) {
        case CompareOp::Never:         psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_NEVER; break;
        case CompareOp::Less:          psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS; break;
        case CompareOp::LessEqual:     psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; break;
        case CompareOp::Greater:       psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER; break;
        case CompareOp::GreaterEqual:  psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL; break;
        case CompareOp::Equal:         psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_EQUAL; break;
        case CompareOp::Always:        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS; break;
    }

    D3D12_RENDER_TARGET_BLEND_DESC& rtBlend = psoDesc.BlendState.RenderTarget[0];
    rtBlend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    rtBlend.BlendEnable = desc.blendState.blendEnable;
    if (desc.blendState.blendEnable) {
        rtBlend.SrcBlend = ToD3D12Blend(desc.blendState.srcColorFactor);
        rtBlend.DestBlend = ToD3D12Blend(desc.blendState.dstColorFactor);
        rtBlend.BlendOp = ToD3D12BlendOp(desc.blendState.colorBlendOp);
        rtBlend.SrcBlendAlpha = ToD3D12Blend(desc.blendState.srcAlphaFactor);
        rtBlend.DestBlendAlpha = ToD3D12Blend(desc.blendState.dstAlphaFactor);
        rtBlend.BlendOpAlpha = ToD3D12BlendOp(desc.blendState.alphaBlendOp);
    }

    psoDesc.SampleMask = UINT_MAX;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.NumRenderTargets = static_cast<uint32_t>(desc.colorTargetFormats.size());
    for (size_t i = 0; i < desc.colorTargetFormats.size(); ++i) {
        psoDesc.RTVFormats[i] = ToDxgiFormat(desc.colorTargetFormats[i]);
    }
    psoDesc.DSVFormat = ToDxgiFormat(desc.depthTargetFormat);

    HR_CHECK(m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&native.pso)));
    return m_pipelines.Add(native);
}

// ============================================================================
// Ray tracing pipeline + shader binding table
// ============================================================================

PipelineHandle D3D12GraphicsDevice::CreateRayTracingPipeline(const RayTracingPipelineDesc& desc) {
    NativePipeline native;
    native.isRayTracing = true;
    RootSignatureInfo rsInfo = BuildRootSignature(desc.bindGroupLayouts);
    native.rootSignature = rsInfo.rootSignature;
    native.setResourceRootParam = rsInfo.setResourceParam;
    native.setSamplerRootParam = rsInfo.setSamplerParam;

    // DXR requires unique export names within the state object -- app-supplied
    // entry points might collide (e.g. two shaders both named "main"), so
    // disambiguate with the shader-module index.
    std::vector<std::wstring> wExportNames(desc.shaderModules.size());
    std::vector<std::wstring> wEntryPoints(desc.shaderModules.size());
    for (uint32_t i = 0; i < desc.shaderModules.size(); ++i) {
        NativeShaderModule& mod = m_shaderModules.Get(desc.shaderModules[i]);
        std::string exportName = mod.entryPoint + "_" + std::to_string(i);
        wExportNames[i] = std::wstring(exportName.begin(), exportName.end());
        wEntryPoints[i] = std::wstring(mod.entryPoint.begin(), mod.entryPoint.end());
    }

    std::vector<D3D12_EXPORT_DESC> exportDescs(desc.shaderModules.size());
    std::vector<D3D12_DXIL_LIBRARY_DESC> libDescs(desc.shaderModules.size());
    for (uint32_t i = 0; i < desc.shaderModules.size(); ++i) {
        NativeShaderModule& mod = m_shaderModules.Get(desc.shaderModules[i]);
        exportDescs[i].Name = wExportNames[i].c_str();
        exportDescs[i].ExportToRename = wEntryPoints[i].c_str();
        exportDescs[i].Flags = D3D12_EXPORT_FLAG_NONE;
        libDescs[i].DXILLibrary = { mod.bytecode.data(), mod.bytecode.size() };
        libDescs[i].NumExports = 1;
        libDescs[i].pExports = &exportDescs[i];
    }

    // One D3D12_HIT_GROUP_DESC per TrianglesHitGroup entry, exported under a
    // synthesized name -- this is the name TLASInstanceDesc::hitGroupIndex
    // effectively selects via the SBT's hit-group region ordering below.
    std::vector<std::wstring> wHitGroupNames;
    for (const auto& g : desc.shaderGroups) {
        if (g.type == ShaderGroupType::TrianglesHitGroup) {
            wHitGroupNames.push_back(L"HitGroup_" + std::to_wstring(wHitGroupNames.size()));
        }
    }
    std::vector<D3D12_HIT_GROUP_DESC> hitGroupDescs;
    hitGroupDescs.reserve(wHitGroupNames.size());
    uint32_t hgIndex = 0;
    for (const auto& g : desc.shaderGroups) {
        if (g.type != ShaderGroupType::TrianglesHitGroup) continue;
        D3D12_HIT_GROUP_DESC hg{};
        hg.HitGroupExport = wHitGroupNames[hgIndex].c_str();
        hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg.ClosestHitShaderImport = wExportNames[g.closestHitShaderIndex].c_str();
        hitGroupDescs.push_back(hg);
        hgIndex++;
    }

    // Now exposed on RayTracingPipelineDesc (Vulkan ignores these two fields
    // -- see the comment on RayTracingPipelineDesc in GraphicsTypes.h).
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
    shaderConfig.MaxPayloadSizeInBytes = desc.maxPayloadSizeBytes;
    shaderConfig.MaxAttributeSizeInBytes = desc.maxAttributeSizeBytes;

    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
    pipelineConfig.MaxTraceRecursionDepth = desc.maxRecursionDepth;

    D3D12_GLOBAL_ROOT_SIGNATURE globalRootSig{ native.rootSignature.Get() };

    std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    subobjects.reserve(libDescs.size() + hitGroupDescs.size() + 3);
    for (auto& lib : libDescs) subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib });
    for (auto& hg : hitGroupDescs) subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg });
    subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig });
    subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig });
    subobjects.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRootSig });
    // No per-export subobject-to-exports associations: with exactly one
    // shader-config/pipeline-config/root-signature subobject each and no
    // local root signatures, DXR implicitly applies them to every export.

    D3D12_STATE_OBJECT_DESC stateObjectDesc{};
    stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
    stateObjectDesc.NumSubobjects = static_cast<uint32_t>(subobjects.size());
    stateObjectDesc.pSubobjects = subobjects.data();

    HR_CHECK(m_device->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&native.rtStateObject)));
    HR_CHECK(native.rtStateObject.As(&native.rtProps));

    // --- Shader binding table ---
    std::vector<uint32_t> raygenIndices, missIndices; // indices into desc.shaderGroups
    for (uint32_t i = 0; i < desc.shaderGroups.size(); ++i) {
        const auto& g = desc.shaderGroups[i];
        if (g.type != ShaderGroupType::General) continue;
        ShaderStage stage = m_shaderModules.Get(desc.shaderModules[g.generalShaderIndex]).stage;
        (stage == ShaderStage::RayGen ? raygenIndices : missIndices).push_back(i);
    }

    constexpr uint32_t kIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    auto alignUp = [](uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); };
    uint32_t recordStride = alignUp(kIdSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);

    uint32_t raygenSize = alignUp(static_cast<uint32_t>(raygenIndices.size()) * recordStride,
                                   D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    uint32_t missSize = alignUp(static_cast<uint32_t>(missIndices.size()) * recordStride,
                                 D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    uint32_t hitSize = alignUp(static_cast<uint32_t>(hitGroupDescs.size()) * recordStride,
                                D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
    uint32_t sbtSize = raygenSize + missSize + hitSize;

    native.sbtBuffer = AllocateBuffer(sbtSize, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);

    uint8_t* mapped = nullptr;
    D3D12_RANGE readRange{ 0, 0 };
    HR_CHECK(native.sbtBuffer.resource->Map(0, &readRange, reinterpret_cast<void**>(&mapped)));

    uint32_t offset = 0;
    for (uint32_t idx : raygenIndices) {
        void* id = native.rtProps->GetShaderIdentifier(wExportNames[desc.shaderGroups[idx].generalShaderIndex].c_str());
        std::memcpy(mapped + offset, id, kIdSize);
        offset += recordStride;
    }
    offset = raygenSize;
    for (uint32_t idx : missIndices) {
        void* id = native.rtProps->GetShaderIdentifier(wExportNames[desc.shaderGroups[idx].generalShaderIndex].c_str());
        std::memcpy(mapped + offset, id, kIdSize);
        offset += recordStride;
    }
    offset = raygenSize + missSize;
    for (const auto& name : wHitGroupNames) {
        void* id = native.rtProps->GetShaderIdentifier(name.c_str());
        std::memcpy(mapped + offset, id, kIdSize);
        offset += recordStride;
    }
    native.sbtBuffer.resource->Unmap(0, nullptr);

    D3D12_GPU_VIRTUAL_ADDRESS sbtAddress = native.sbtBuffer.gpuAddress;
    native.raygenRange = { sbtAddress, raygenSize };
    native.missRange = { sbtAddress + raygenSize, missSize, recordStride };
    native.hitRange = { sbtAddress + raygenSize + missSize, hitSize, recordStride };

    return m_pipelines.Add(native);
}

void D3D12GraphicsDevice::DestroyPipeline(PipelineHandle handle) {
    m_pipelines.Remove(handle); // ComPtr/NativeBuffer members release automatically
}

// ============================================================================
// Acceleration structures
// ============================================================================
// D3D12 acceleration structures are just UAV buffers in the
// RAYTRACING_ACCELERATION_STRUCTURE state -- no separate "create AS object"
// call like Vulkan's vkCreateAccelerationStructureKHR; the buffer's GPU VA
// alone identifies it. Simpler than Vulkan here specifically because of that.

D3D12_RAYTRACING_GEOMETRY_DESC D3D12GraphicsDevice::ToGeometry(const BLASGeometryDesc& geom,
                                                                uint32_t& outPrimitiveCount) {
    NativeBuffer& vbuf = m_buffers.Get(geom.vertexBuffer);
    NativeBuffer& ibuf = m_buffers.Get(geom.indexBuffer);

    D3D12_RAYTRACING_GEOMETRY_DESC g{};
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags = geom.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
    g.Triangles.VertexBuffer.StartAddress = vbuf.gpuAddress;
    g.Triangles.VertexBuffer.StrideInBytes = geom.vertexStride;
    g.Triangles.VertexCount = geom.vertexCount;
    g.Triangles.VertexFormat = ToDxgiFormat(geom.vertexFormat);
    g.Triangles.IndexBuffer = ibuf.gpuAddress;
    g.Triangles.IndexCount = geom.indexCount;
    g.Triangles.IndexFormat = geom.use32BitIndices ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;

    outPrimitiveCount = geom.indexCount / 3;
    return g;
}

BLASHandle D3D12GraphicsDevice::CreateBLAS(const BLASBuildDesc& desc) {
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    for (const auto& g : desc.geometries) {
        uint32_t primCount = 0;
        geometries.push_back(ToGeometry(g, primCount));
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = static_cast<uint32_t>(geometries.size());
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = geometries.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    NativeAccelStruct native;
    NativeBuffer buf = AllocateBuffer(prebuild.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    native.backingBuffer = buf.resource;
    native.address = buf.gpuAddress;
    return m_blases.Add(native);
}

void D3D12GraphicsDevice::DestroyBLAS(BLASHandle handle) {
    m_blases.Remove(handle);
}

TLASHandle D3D12GraphicsDevice::CreateTLAS(const TLASBuildDesc& desc) {
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = static_cast<uint32_t>(desc.instances.size());
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    // Note: prebuild sizing for a TLAS only needs the instance *count*, not
    // the actual instance buffer -- that's supplied later, at build time.

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    m_device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    NativeAccelStruct native;
    NativeBuffer buf = AllocateBuffer(prebuild.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                       D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    native.backingBuffer = buf.resource;
    native.address = buf.gpuAddress;
    return m_tlases.Add(native);
}

void D3D12GraphicsDevice::DestroyTLAS(TLASHandle handle) {
    m_tlases.Remove(handle);
}

// ============================================================================
// Swapchain
// ============================================================================

SwapchainHandle D3D12GraphicsDevice::CreateSwapchain(const SwapchainDesc& desc) {
    NativeSwapchain native;
    native.format = ToDxgiFormat(desc.format);
    native.width = desc.width;
    native.height = desc.height;
    native.bufferCount = desc.bufferCount;

    DXGI_SWAP_CHAIN_DESC1 scDesc{};
    scDesc.Width = desc.width;
    scDesc.Height = desc.height;
    scDesc.Format = native.format;
    scDesc.SampleDesc.Count = 1;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.BufferCount = desc.bufferCount;
    scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // required for the flip model; no legacy blt-model support here

    ComPtr<IDXGISwapChain1> sc1;
    HR_CHECK(m_factory->CreateSwapChainForHwnd(m_queue.Get(), static_cast<HWND>(desc.windowHandle), &scDesc,
                                                nullptr, nullptr, &sc1));
    HR_CHECK(sc1.As(&native.swapchain));

    HR_CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&native.frameFence)));
    native.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    native.bufferFenceValues.resize(desc.bufferCount, 0);

    SwapchainHandle handle = m_swapchains.Add(native);
    NativeSwapchain& sc = m_swapchains.Get(handle);

    for (uint32_t i = 0; i < desc.bufferCount; ++i) {
        ComPtr<ID3D12Resource> backBuffer;
        HR_CHECK(sc.swapchain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));

        NativeTexture tex;
        tex.resource = backBuffer;
        tex.format = native.format;
        tex.width = desc.width;
        tex.height = desc.height;
        tex.isSwapchainImage = true;
        tex.owningSwapchain = handle;

        uint32_t rtvIdx = m_rtvHeap.Allocate(1);
        tex.rtvHandle = m_rtvHeap.CpuHandle(rtvIdx);
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, tex.rtvHandle);
        tex.hasRtv = true;

        sc.imageTextures.push_back(m_textures.Add(tex));
    }

    return handle;
}

void D3D12GraphicsDevice::DestroySwapchain(SwapchainHandle handle) {
    if (!m_swapchains.IsValid(handle)) return;
    WaitIdle();
    NativeSwapchain& sc = m_swapchains.Get(handle);
    for (auto t : sc.imageTextures) m_textures.Remove(t);
    if (sc.fenceEvent) CloseHandle(sc.fenceEvent);
    m_swapchains.Remove(handle);
}

void D3D12GraphicsDevice::ResizeSwapchain(SwapchainHandle handle, uint32_t width, uint32_t height) {
    NativeSwapchain& sc = m_swapchains.Get(handle);
    WaitIdle();
    // All references to swapchain buffers (RTVs, NativeTexture entries) must
    // be released before ResizeBuffers -- DXGI refuses to resize otherwise.
    for (auto t : sc.imageTextures) m_textures.Remove(t);
    sc.imageTextures.clear();

    HR_CHECK(sc.swapchain->ResizeBuffers(sc.bufferCount, width, height, sc.format, 0));
    sc.width = width;
    sc.height = height;
    std::fill(sc.bufferFenceValues.begin(), sc.bufferFenceValues.end(), 0);

    for (uint32_t i = 0; i < sc.bufferCount; ++i) {
        ComPtr<ID3D12Resource> backBuffer;
        HR_CHECK(sc.swapchain->GetBuffer(i, IID_PPV_ARGS(&backBuffer)));
        NativeTexture tex;
        tex.resource = backBuffer;
        tex.format = sc.format;
        tex.width = width;
        tex.height = height;
        tex.isSwapchainImage = true;
        tex.owningSwapchain = handle;
        uint32_t rtvIdx = m_rtvHeap.Allocate(1);
        tex.rtvHandle = m_rtvHeap.CpuHandle(rtvIdx);
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, tex.rtvHandle);
        tex.hasRtv = true;
        sc.imageTextures.push_back(m_textures.Add(tex));
    }
}

TextureHandle D3D12GraphicsDevice::AcquireNextSwapchainTexture(SwapchainHandle handle) {
    NativeSwapchain& sc = m_swapchains.Get(handle);
    uint32_t index = sc.swapchain->GetCurrentBackBufferIndex();

    // Unlike Vulkan's semaphore-blocking vkAcquireNextImageKHR, D3D12 hands
    // back the index immediately -- but if this buffer's previous frame
    // hasn't finished rendering yet (CPU running far ahead of GPU), wait
    // here rather than let Present race a still-in-flight resource.
    uint64_t neededValue = sc.bufferFenceValues[index];
    if (neededValue != 0 && sc.frameFence->GetCompletedValue() < neededValue) {
        HR_CHECK(sc.frameFence->SetEventOnCompletion(neededValue, sc.fenceEvent));
        WaitForSingleObject(sc.fenceEvent, INFINITE);
    }

    return sc.imageTextures[index];
}

void D3D12GraphicsDevice::Present(SwapchainHandle handle) {
    NativeSwapchain& sc = m_swapchains.Get(handle);
    HR_CHECK(sc.swapchain->Present(1, 0)); // vsync'd; use (0, DXGI_PRESENT_ALLOW_TEARING) for uncapped+tearing
}

// ============================================================================
// Command recording / submission
// ============================================================================

ICommandList* D3D12GraphicsDevice::BeginCommandList() {
    ComPtr<ID3D12CommandAllocator> allocator;
    HR_CHECK(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));

    ComPtr<ID3D12GraphicsCommandList4> cmdList;
    HR_CHECK(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&cmdList)));

    ID3D12DescriptorHeap* heaps[] = { m_cbvSrvUavHeap.heap.Get(), m_samplerHeap.heap.Get() };
    cmdList->SetDescriptorHeaps(2, heaps);

    auto list = std::make_unique<D3D12CommandList>(this, allocator, cmdList);
    ICommandList* ptr = list.get();
    m_liveCommandLists.push_back(std::move(list));
    return ptr;
}

FenceHandle D3D12GraphicsDevice::Submit(ICommandList* commandList) {
    auto* d3dList = static_cast<D3D12CommandList*>(commandList);
    HR_CHECK(d3dList->GetCommandList()->Close());

    ID3D12CommandList* lists[] = { d3dList->GetCommandList() };
    m_queue->ExecuteCommandLists(1, lists);

    NativeFence nativeFence;
    HR_CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&nativeFence.fence)));
    nativeFence.value = 1;
    nativeFence.event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    HR_CHECK(m_queue->Signal(nativeFence.fence.Get(), nativeFence.value));

    // If this list transitioned a swapchain image to Present, also signal
    // that swapchain's own frame fence so a future AcquireNextSwapchainTexture
    // for this same buffer index knows when it's safe to reuse.
    SwapchainHandle usedSwapchain = d3dList->GetPresentedSwapchain();
    if (usedSwapchain.IsValid() && m_swapchains.IsValid(usedSwapchain)) {
        NativeSwapchain& sc = m_swapchains.Get(usedSwapchain);
        uint64_t frameValue = sc.nextFenceValue++;
        HR_CHECK(m_queue->Signal(sc.frameFence.Get(), frameValue));
        uint32_t idx = d3dList->GetPresentedImageIndex();
        if (idx < sc.bufferFenceValues.size()) sc.bufferFenceValues[idx] = frameValue;
    }

    auto it = std::find_if(m_liveCommandLists.begin(), m_liveCommandLists.end(),
                            [&](auto& l) { return l.get() == commandList; });
    if (it != m_liveCommandLists.end()) {
        m_pendingCommandLists[nativeFence.fence.Get()] = { nativeFence.value, std::move(*it) };
        m_liveCommandLists.erase(it);
    }

    return m_fences.Add(nativeFence);
}

void D3D12GraphicsDevice::WaitForFence(FenceHandle fence) {
    if (!m_fences.IsValid(fence)) return;
    NativeFence& f = m_fences.Get(fence);
    if (f.fence->GetCompletedValue() < f.value) {
        HR_CHECK(f.fence->SetEventOnCompletion(f.value, f.event));
        WaitForSingleObject(f.event, INFINITE);
    }
    m_pendingCommandLists.erase(f.fence.Get()); // safe to free scratch buffers/allocator now
    CloseHandle(f.event);
    m_fences.Remove(fence);
}

void D3D12GraphicsDevice::WaitIdle() {
    // D3D12 has no direct vkDeviceWaitIdle equivalent -- signal-and-wait on
    // a throwaway fence is the standard idiom.
    ComPtr<ID3D12Fence> fence;
    HR_CHECK(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    HR_CHECK(m_queue->Signal(fence.Get(), 1));
    HR_CHECK(fence->SetEventOnCompletion(1, event));
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);
    m_pendingCommandLists.clear();
}

} // namespace draw::d3d12_backend