#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace spray::graphics {

enum class BackendType {
    Vulkan,
    D3D12
};

// ============================================================================
// Opaque handles
// ============================================================================
// All handles are backend-agnostic IDs. Each IDevice implementation
// maps these to its own native objects (VkBuffer/ID3D12Resource, etc.) in an
// internal table. A generation counter catches stale-handle use (e.g. handle
// held across a backend switch, or after Destroy*).

template <typename Tag>
struct Handle {
    uint32_t index = 0;
    uint32_t generation = 0;

    bool IsValid() const { return generation != 0; }
    friend bool operator==(const Handle&, const Handle&) = default;
};

struct BufferTag {};
struct TextureTag {};
struct ShaderModuleTag {};
struct PipelineTag {};
struct BindGroupLayoutTag {};
struct BindGroupTag {};
struct SwapchainTag {};
struct CommandListTag {};
struct FenceTag {};
struct BLASTag {};
struct TLASTag {};
struct SamplerTag {};

using BufferHandle = Handle<BufferTag>;
using TextureHandle = Handle<TextureTag>;
using ShaderModuleHandle = Handle<ShaderModuleTag>;
using PipelineHandle = Handle<PipelineTag>;
using BindGroupLayoutHandle = Handle<BindGroupLayoutTag>;
using BindGroupHandle = Handle<BindGroupTag>;
using SwapchainHandle = Handle<SwapchainTag>;
using CommandListHandle = Handle<CommandListTag>;
using FenceHandle = Handle<FenceTag>;
using BLASHandle = Handle<BLASTag>;
using TLASHandle = Handle<TLASTag>;
using SamplerHandle = Handle<SamplerTag>;

// ============================================================================
// Formats
// ============================================================================
// Own enum, translated to VkFormat / DXGI_FORMAT internally per backend.

enum class Format {
    Unknown,
    RGBA8_UNorm,
    BGRA8_UNorm,
    RGBA16_Float,
    RGBA32_Float,
    RGB32_Float, // vertex positions/normals -- tightly packed vec3 attributes
    RG32_Float,  // vertex UVs -- tightly packed vec2 attributes
    R32_Float,
    D32_Float,
    D24_UNorm_S8_UInt,
};

// ============================================================================
// Resource descriptions
// ============================================================================

enum class BufferUsage : uint32_t {
    None = 0,
    VertexBuffer = 1 << 0,
    IndexBuffer = 1 << 1,
    UniformBuffer = 1 << 2,
    StorageBuffer = 1 << 3,
    CopySrc = 1 << 4,
    CopyDst = 1 << 5,
};
inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool HasFlag(BufferUsage value, BufferUsage flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

struct BufferDesc {
    size_t sizeBytes = 0;
    BufferUsage usage = BufferUsage::None;
    bool hostVisible = false; // CPU-writable (upload heap / HOST_VISIBLE memory)
    std::string debugName;
};

enum class TextureUsage : uint32_t {
    None = 0,
    RenderTarget = 1 << 0,
    DepthStencil = 1 << 1,
    ShaderResource = 1 << 2,
    CopySrc = 1 << 3,
    CopyDst = 1 << 4,
    Storage = 1 << 5,
};
inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool HasFlag(TextureUsage value, TextureUsage flag) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

enum class TextureDimension {
    Texture2D,
    Texture2DArray,
    TextureCube,  // always 6 faces; cube arrays aren't supported yet
    Texture3D,
};

struct TextureDesc {
    uint32_t width = 1;
    uint32_t height = 1;
    // Meaning depends on dimension: depth for Texture3D, array layer count
    // for Texture2DArray, ignored for TextureCube (always 6) and Texture2D
    // (always 1).
    uint32_t depthOrArrayLayers = 1;
    TextureDimension dimension = TextureDimension::Texture2D;
    Format format = Format::Unknown;
    TextureUsage usage = TextureUsage::None;
    uint32_t mipLevels = 1;
    std::string debugName;
};

enum class FilterMode {
    Nearest,
    Linear,
};

enum class AddressMode {
    Repeat,
    ClampToEdge,
    MirroredRepeat,
};

struct SamplerDesc {
    FilterMode magFilter = FilterMode::Linear;
    FilterMode minFilter = FilterMode::Linear;
    AddressMode addressModeU = AddressMode::Repeat;
    AddressMode addressModeV = AddressMode::Repeat;
    std::string debugName;
};

// ============================================================================
// Resource state (for barriers / transitions)
// ============================================================================
// Both Vulkan (image layouts) and D3D12 (resource states) require explicit
// transitions. This is the shared vocabulary; each backend maps it to its
// native equivalent (e.g. ShaderReadOnly -> VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
// or D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE).

enum class ResourceState {
    Undefined,
    RenderTarget,
    DepthWrite,
    DepthRead,
    ShaderReadOnly,
    General,
    CopySrc,
    CopyDst,
    Present,
};

struct TextureBarrier {
    TextureHandle texture;
    ResourceState before;
    ResourceState after;
};

// ============================================================================
// Render target attachments (for ICommandList::BeginRendering)
// ============================================================================
// Both clear and load are expressed here rather than as a separate call --
// a separate "ClearColorTarget" call would need its own barrier/state
// bookkeeping to be correct, whereas folding clear into the attachment
// description matches how both Vulkan (VkAttachmentLoadOp) and D3D12
// (ClearRenderTargetView called once, right before the resource is bound)
// actually express it.

struct ColorAttachment {
    TextureHandle texture;
    bool clear = false;
    float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
};

struct DepthAttachment {
    TextureHandle texture; // default-constructed (invalid) means no depth target
    bool clear = false;
    float clearDepth = 1.0f;
    uint8_t clearStencil = 0;
};

// ============================================================================
// Shaders
// ============================================================================
// The app supplies pre-compiled bytecode for whichever backends it targets.
// Compiling GLSL/HLSL -> SPIR-V/DXIL is a build-step concern, not runtime API
// scope. The device picks whichever blob matches its own backend type and
// ignores the other.

enum class ShaderStage {
    Vertex,
    Pixel,
    Compute,
    RayGen,
    Miss,
    ClosestHit,
    // AnyHit / Intersection deliberately omitted for now -- not needed
    // without procedural primitives or non-opaque geometry. Add if/when
    // the path tracer needs alpha-tested geometry or procedural shapes.
};

struct ShaderBytecode {
    std::vector<uint8_t> spirv; // used when device backend == Vulkan
    std::vector<uint8_t> dxil;  // used when device backend == D3D12
};

struct ShaderModuleDesc {
    ShaderStage stage;
    ShaderBytecode bytecode;
    std::string entryPoint = "main"; // D3D12 conventionally uses this; Vulkan honors it too
    std::string debugName;
};

// ============================================================================
// Bind groups (resource binding)
// ============================================================================
// Abstracts Vulkan descriptor sets/layouts and D3D12 root signatures +
// descriptor heaps behind one shared model: a BindGroupLayout declares what
// slots exist, a BindGroup is a concrete set of resources bound to those
// slots. Each backend implementation lowers this to its native mechanism
// internally (Vulkan: VkDescriptorSetLayout/VkDescriptorSet backed by a pool;
// D3D12: root signature built from all layouts a pipeline uses + a CBV/SRV/UAV
// heap allocation per BindGroup).

enum class BindingType {
    UniformBuffer,
    StorageBuffer,
    SampledTexture,
    StorageTexture,
    Sampler,
    AccelerationStructure,
};

struct BindGroupLayoutEntry {
    uint32_t binding;
    BindingType type;
    ShaderStage visibleStage;
};

struct BindGroupLayoutDesc {
    std::vector<BindGroupLayoutEntry> entries;
    std::string debugName;
};

struct BindGroupEntry {
    uint32_t binding;
    // Exactly one of these should be valid, matching the layout entry's type.
    BufferHandle buffer;
    TextureHandle texture;
    TLASHandle accelerationStructure;
    SamplerHandle sampler;

    // Named constructors -- prefer these over aggregate init. The struct's
    // field order doesn't match how you'd naturally list arguments for,
    // say, a sampler entry (you'd otherwise write
    // `{ binding, {}, {}, {}, samplerHandle }`); these skip that.
    static BindGroupEntry Buffer(uint32_t binding, BufferHandle handle) {
        BindGroupEntry e; e.binding = binding; e.buffer = handle; return e;
    }
    static BindGroupEntry Texture(uint32_t binding, TextureHandle handle) {
        BindGroupEntry e; e.binding = binding; e.texture = handle; return e;
    }
    static BindGroupEntry AccelStruct(uint32_t binding, TLASHandle handle) {
        BindGroupEntry e; e.binding = binding; e.accelerationStructure = handle; return e;
    }
    static BindGroupEntry Sampler(uint32_t binding, SamplerHandle handle) {
        BindGroupEntry e; e.binding = binding; e.sampler = handle; return e;
    }
};

struct BindGroupDesc {
    BindGroupLayoutHandle layout;
    std::vector<BindGroupEntry> entries;
    std::string debugName;
};

// ============================================================================
// Pipeline state
// ============================================================================

enum class PrimitiveTopology {
    TriangleList,
    LineList,
    PointList,
};

enum class CullMode {
    None,
    Front,
    Back,
};

enum class CompareOp {
    Never,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
    Always,
};

struct VertexAttribute {
    uint32_t location;
    Format format;
    uint32_t offset;
};

struct VertexBufferLayout {
    uint32_t stride;
    std::vector<VertexAttribute> attributes;
};

struct RasterizerState {
    CullMode cullMode = CullMode::Back;
    bool wireframe = false;
};

enum class BlendFactor {
    Zero,
    One,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
};

enum class BlendOp {
    Add,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
};

struct BlendState {
    bool blendEnable = false;
    BlendFactor srcColorFactor = BlendFactor::SrcAlpha;
    BlendFactor dstColorFactor = BlendFactor::OneMinusSrcAlpha;
    BlendOp colorBlendOp = BlendOp::Add;
    BlendFactor srcAlphaFactor = BlendFactor::One;
    BlendFactor dstAlphaFactor = BlendFactor::Zero;
    BlendOp alphaBlendOp = BlendOp::Add;
};

struct DepthStencilState {
    bool depthTestEnable = true;
    bool depthWriteEnable = true;
    CompareOp depthCompareOp = CompareOp::Less;
};

struct GraphicsPipelineDesc {
    ShaderModuleHandle vertexShader;
    ShaderModuleHandle pixelShader;

    std::vector<VertexBufferLayout> vertexBuffers;
    std::vector<BindGroupLayoutHandle> bindGroupLayouts; // in set/space order

    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    RasterizerState rasterizer;
    DepthStencilState depthStencil;
    BlendState blendState; // disabled (opaque) by default

    std::vector<Format> colorTargetFormats;
    Format depthTargetFormat = Format::Unknown;

    std::string debugName;
};

// ============================================================================
// Ray tracing: acceleration structures
// ============================================================================
// DXR and VK_KHR_ray_tracing_pipeline are unusually symmetric here -- both
// build a two-level structure: one BLAS per mesh (geometry only, no
// transform baked in beyond what you provide), and one TLAS referencing
// BLAS instances with per-instance transforms. This shared model maps onto
// both with little translation needed.
//
// Build is a two-step process, mirroring how both APIs actually work:
//   1. CreateBLAS/CreateTLAS on the device -- queries the backend for
//      required buffer size (vkGetAccelerationStructureBuildSizesKHR /
//      GetRaytracingAccelerationStructurePrebuildInfo) and allocates the
//      backing buffer. Returns a valid handle, but the structure is not
//      yet built.
//   2. ICommandList::BuildBLAS/BuildTLAS -- records the actual GPU build
//      command against that handle. Must complete (via the returned fence,
//      or a subsequent WaitIdle) before the structure is used in a
//      TraceRays call or referenced by a TLAS build.
//
// Same BLASBuildDesc/TLASBuildDesc is passed to both calls.

struct BLASGeometryDesc {
    BufferHandle vertexBuffer;
    uint32_t vertexStride = 0;
    Format vertexFormat = Format::RGB32_Float;
    uint32_t vertexCount = 0;

    BufferHandle indexBuffer;
    uint32_t indexCount = 0;
    bool use32BitIndices = true;

    bool opaque = true; // no any-hit shader will run against this geometry
};

struct BLASBuildDesc {
    std::vector<BLASGeometryDesc> geometries;
    std::string debugName;
};

struct TLASInstanceDesc {
    BLASHandle blas;
    float transform[12] = { // row-major 3x4, matches both VkTransformMatrixKHR and D3D12_RAYTRACING_INSTANCE_DESC
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
    };
    uint32_t instanceID = 0;      // available to shaders as InstanceID / InstanceIndex
    uint32_t hitGroupIndex = 0;   // selects which shader group in the SBT this instance hits
    uint8_t mask = 0xFF;
};

struct TLASBuildDesc {
    std::vector<TLASInstanceDesc> instances;
    std::string debugName;
};

// ============================================================================
// Ray tracing pipeline
// ============================================================================
// A ray tracing pipeline is a set of shader modules plus a grouping of them
// into "shader groups" -- each raygen/miss shader is its own group; each hit
// group bundles a closest-hit shader (any-hit/intersection omitted for now,
// see ShaderStage). Group order determines shader binding table layout;
// TLASInstanceDesc::hitGroupIndex indexes into the hit groups specifically
// (not the full shaderGroups list) in the order they appear here.
//
// The shader binding table itself is built internally by the device inside
// CreateRayTracingPipeline -- the app never touches raw shader identifiers
// or SBT strides.

enum class ShaderGroupType {
    General,           // raygen or miss shader
    TrianglesHitGroup, // closest-hit (+ any-hit/intersection, once supported)
};

struct ShaderGroupDesc {
    ShaderGroupType type;
    uint32_t generalShaderIndex = UINT32_MAX;    // index into RayTracingPipelineDesc::shaderModules, for General
    uint32_t closestHitShaderIndex = UINT32_MAX; // index into shaderModules, for TrianglesHitGroup
};

struct RayTracingPipelineDesc {
    std::vector<ShaderModuleHandle> shaderModules;
    std::vector<ShaderGroupDesc> shaderGroups;
    uint32_t maxRecursionDepth = 1;
    // Max size of the ray payload struct / hit attribute struct your shaders
    // declare. D3D12 requires this up front (D3D12_RAYTRACING_SHADER_CONFIG);
    // Vulkan doesn't need it for a standalone (non-library) pipeline like this
    // one -- shader-declared payload sizes are enough there, so the Vulkan
    // backend ignores these two fields.
    uint32_t maxPayloadSizeBytes = 64;
    uint32_t maxAttributeSizeBytes = 8; // 8 = built-in triangle barycentrics (float2); raise only for custom intersection shaders
    std::vector<BindGroupLayoutHandle> bindGroupLayouts;
    std::string debugName;
};

// ============================================================================
// Compute pipeline
// ============================================================================

struct ComputePipelineDesc {
    ShaderModuleHandle computeShader; // ShaderModuleDesc::stage must be ShaderStage::Compute
    std::vector<BindGroupLayoutHandle> bindGroupLayouts;
    std::string debugName;
};

} // namespace spray::graphics