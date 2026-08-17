#include "pch.hpp"
#include "GltfImporter.hpp"

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <tiny_gltf.h>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/matrix_decompose.hpp>

#include <cstring>

namespace spray::assets {

namespace {

glm::mat4 NodeLocalTransform(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        return glm::make_mat4(node.matrix.data());
    }
    glm::vec3 t{0.0f}, s{1.0f};
    glm::quat r{1.0f, 0.0f, 0.0f, 0.0f};
    if (node.translation.size() == 3) t = glm::make_vec3(node.translation.data());
    if (node.rotation.size() == 4) {
        // glTF stores quaternions as [x, y, z, w]; glm::quat's constructor
        // wants (w, x, y, z).
        r = glm::quat(static_cast<float>(node.rotation[3]), static_cast<float>(node.rotation[0]),
                      static_cast<float>(node.rotation[1]), static_cast<float>(node.rotation[2]));
    }
    if (node.scale.size() == 3) s = glm::make_vec3(node.scale.data());
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

const float* ReadFloatAccessor(const tinygltf::Model& model, const tinygltf::Primitive& prim,
                                const std::string& attrName) {
    auto it = prim.attributes.find(attrName);
    if (it == prim.attributes.end()) return nullptr;
    const auto& accessor = model.accessors[it->second];
    const auto& view = model.bufferViews[accessor.bufferView];
    const auto& buf = model.buffers[view.buffer];
    return reinterpret_cast<const float*>(&buf.data[view.byteOffset + accessor.byteOffset]);
}

MeshAsset ImportMeshPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& prim) {
    MeshAsset mesh;

    auto posIt = prim.attributes.find("POSITION");
    if (posIt == prim.attributes.end()) return mesh; // no positions -- nothing to import

    const float* positions = ReadFloatAccessor(model, prim, "POSITION");
    const float* normals = ReadFloatAccessor(model, prim, "NORMAL");
    const float* uvs = ReadFloatAccessor(model, prim, "TEXCOORD_0");
    size_t vertexCount = model.accessors[posIt->second].count;

    mesh.vertices.resize(vertexCount);
    for (size_t i = 0; i < vertexCount; ++i) {
        mesh.vertices[i].position = glm::make_vec3(positions + i * 3);
        mesh.vertices[i].normal = normals ? glm::make_vec3(normals + i * 3) : glm::vec3(0.0f, 1.0f, 0.0f);
        mesh.vertices[i].uv = uvs ? glm::make_vec2(uvs + i * 2) : glm::vec2(0.0f);
    }

    if (prim.indices < 0) return mesh; // non-indexed primitives aren't supported yet

    // Indices: glTF permits u8/u16/u32 component types -- widen to u32
    // uniformly since that's what BufferUsage::IndexBuffer / SetIndexBuffer's
    // use32BitIndices path expects downstream.
    const auto& idxAccessor = model.accessors[prim.indices];
    const auto& idxView = model.bufferViews[idxAccessor.bufferView];
    const auto& idxBuf = model.buffers[idxView.buffer];
    const uint8_t* idxData = &idxBuf.data[idxView.byteOffset + idxAccessor.byteOffset];
    mesh.indices.resize(idxAccessor.count);
    for (size_t i = 0; i < idxAccessor.count; ++i) {
        switch (idxAccessor.componentType) {
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
                mesh.indices[i] = idxData[i];
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
                mesh.indices[i] = reinterpret_cast<const uint16_t*>(idxData)[i];
                break;
            case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
                mesh.indices[i] = reinterpret_cast<const uint32_t*>(idxData)[i];
                break;
            default:
                break; // unsupported component type -- leaves a 0 index, caller sees a malformed mesh
        }
    }
    return mesh;
}

MaterialAsset ImportMaterial(const tinygltf::Model& model, const tinygltf::Material& mat) {
    MaterialAsset out;
    const auto& pbr = mat.pbrMetallicRoughness;
    if (pbr.baseColorFactor.size() == 4) out.baseColorFactor = glm::make_vec4(pbr.baseColorFactor.data());
    out.metallicFactor = static_cast<float>(pbr.metallicFactor);
    out.roughnessFactor = static_cast<float>(pbr.roughnessFactor);
    if (pbr.baseColorTexture.index >= 0) {
        const auto& tex = model.textures[pbr.baseColorTexture.index];
        if (tex.source >= 0) out.baseColorTexturePath = model.images[tex.source].uri;
        // NOTE: this only stores the URI/relative path glTF gives us, not
        // decoded image bytes. Actual texture upload (embedded base64 vs.
        // external file vs. .glb-embedded buffer view) is real work,
        // deliberately left for once mesh-only import is working end to
        // end -- see AssetManager for where GetOrCreateGpuTexture would go
        // alongside GetOrCreateGpuMesh.
    }
    return out;
}

void ImportNodeRecursive(const tinygltf::Model& model, int nodeIndex, Scene& scene, AssetManager& assets,
                          entt::entity parent) {
    const tinygltf::Node& node = model.nodes[nodeIndex];
    entt::entity e = scene.CreateEntity(node.name.empty() ? "Node" : node.name, parent);

    glm::mat4 local = NodeLocalTransform(node);
    Transform& xf = scene.GetRegistry().get<Transform>(e);
    glm::vec3 skew;
    glm::vec4 perspective;
    // glTF node matrices are always affine, so a straightforward decompose
    // is safe here (no shear/perspective components to worry about losing).
    glm::decompose(local, xf.scale, xf.rotation, xf.position, skew, perspective);

    if (node.mesh >= 0) {
        const tinygltf::Mesh& gltfMesh = model.meshes[node.mesh];
        // One glTF "mesh" can have multiple primitives (= multiple
        // materials); each becomes its own MeshRenderer entity so
        // MeshRenderer stays single-mesh/single-material. No dedup against
        // repeated mesh indices across nodes -- Sketchfab-style single-object
        // downloads rarely reuse meshes; revisit with a gltf-mesh-index ->
        // AssetHandle cache if that assumption stops holding for your content.
        for (size_t p = 0; p < gltfMesh.primitives.size(); ++p) {
            const auto& prim = gltfMesh.primitives[p];
            entt::entity primEntity = (gltfMesh.primitives.size() == 1)
                ? e
                : scene.CreateEntity(node.name + "_prim" + std::to_string(p), e);

            AssetHandle<MeshTag> meshHandle = assets.AddMesh(ImportMeshPrimitive(model, prim));
            AssetHandle<MaterialTag> matHandle = (prim.material >= 0)
                ? assets.AddMaterial(ImportMaterial(model, model.materials[prim.material]))
                : AssetHandle<MaterialTag>{}; // invalid = renderer falls back to a default material

            scene.GetRegistry().emplace<MeshRenderer>(primEntity, meshHandle, matHandle);
        }
    }

    for (int child : node.children) {
        ImportNodeRecursive(model, child, scene, assets, e);
    }
}

} // namespace

bool ImportGltf(const std::filesystem::path& path, Scene& scene, AssetManager& assets, std::string& outError) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string warn;

    bool isBinary = path.extension() == ".glb";
    bool ok = isBinary
        ? loader.LoadBinaryFromFile(&model, &outError, &warn, path.string())
        : loader.LoadASCIIFromFile(&model, &outError, &warn, path.string());
    if (!ok) return false;

    if (model.scenes.empty()) {
        outError = "glTF file has no scenes: " + path.string();
        return false;
    }
    const tinygltf::Scene& gltfScene = model.scenes[model.defaultScene >= 0 ? model.defaultScene : 0];

    for (int rootNode : gltfScene.nodes) {
        ImportNodeRecursive(model, rootNode, scene, assets, entt::null);
    }

    scene.name = path.stem().string();
    scene.sourcePath = path;
    return true;
}

} // namespace spray::assets