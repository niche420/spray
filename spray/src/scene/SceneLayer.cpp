#include "pch.hpp"
#include "SceneLayer.hpp"
#include "Components.hpp"
#include "asset/GltfImporter.hpp"
#include "event/Input.hpp"
#include "graphics/CommandList.hpp"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <SDL3/SDL_scancode.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <type_traits>

namespace spray {

using namespace event;

namespace {
    // entt::entity is an opaque enum class -- ImGui widget IDs just need
    // *some* stable integral value per entity, so this unwraps it to its
    // underlying type rather than depending on a specific entt helper
    // function name that may differ across versions.
    void* EntityImGuiId(entt::entity e) {
        using Integral = std::underlying_type_t<entt::entity>;
        return reinterpret_cast<void*>(static_cast<uintptr_t>(static_cast<Integral>(e)));
    }
} // namespace

SceneLayer::SceneLayer(graphics::IDevice& device, graphics::Format colorFormat, graphics::Format depthFormat)
    : Layer("Scene"), m_device(device), m_colorFormat(colorFormat), m_depthFormat(depthFormat) {}

SceneLayer::~SceneLayer() {
    // Order matters: both renderers must release their GPU resources before
    // AssetManager's GPU mesh/BLAS caches are invalidated (all three
    // against the same still-alive m_device -- App guarantees WaitIdle
    // before layers are torn down, see App::~App). Order between
    // m_pPathTracer and m_pSceneRenderer themselves doesn't matter -- they
    // don't reference each other, only Scene/AssetManager.
    m_pPathTracer.reset();
    m_pSceneRenderer.reset();
    if (m_pAssets) m_pAssets->InvalidateGpuCache(m_device);
}

void SceneLayer::OnAttach() {
    m_pAssets = std::make_unique<assets::AssetManager>();
    m_pScene = std::make_unique<Scene>();
    m_pSceneRenderer = std::make_unique<graphics::SceneRenderer>(m_device, *m_pAssets);
    m_pPathTracer = std::make_unique<graphics::PathTracer>(m_device, *m_pAssets);

    // Placeholder camera until a real orbit/fly controller with mouse-look
    // exists (Input::ConsumeMouseDelta is there for it) -- WASD/QE
    // movement below is enough to not be a fixed screenshot in the
    // meantime.
    m_activeCamera = m_pScene->CreateEntity("MainCamera");
    auto& registry = m_pScene->GetRegistry();
    registry.emplace<Camera>(m_activeCamera).isPrimary = true;
    registry.get<Transform>(m_activeCamera).position = { 0.0f, 1.0f, 3.0f };

    LoadScene(m_loadPathBuffer);
}

void SceneLayer::LoadScene(const std::string& path) {
    std::string error;
    if (!assets::ImportGltf(path, *m_pScene, *m_pAssets, error)) {
        std::cerr << "SceneLayer: failed to load '" << path << "': " << error << "\n";
    }
}

void SceneLayer::OnUpdate(float deltaSeconds) {
    if (m_activeCamera != entt::null) {
        Transform& camXf = m_pScene->GetRegistry().get<Transform>(m_activeCamera);
        glm::vec3 forward = camXf.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        glm::vec3 right = camXf.rotation * glm::vec3(1.0f, 0.0f, 0.0f);
        float speed = 3.0f * deltaSeconds;
        if (Input::IsKeyDown(SDL_SCANCODE_W)) camXf.position += forward * speed;
        if (Input::IsKeyDown(SDL_SCANCODE_S)) camXf.position -= forward * speed;
        if (Input::IsKeyDown(SDL_SCANCODE_D)) camXf.position += right * speed;
        if (Input::IsKeyDown(SDL_SCANCODE_A)) camXf.position -= right * speed;
        if (Input::IsKeyDown(SDL_SCANCODE_E)) camXf.position.y += speed;
        if (Input::IsKeyDown(SDL_SCANCODE_Q)) camXf.position.y -= speed;
    }

    m_pScene->UpdateWorldTransforms();
}

void SceneLayer::OnEvent(event::Event& e) {
    // Exists for triggers that don't have a direct SceneLayer reference --
    // see LoadScene's comment. The content browser's own Load button below
    // calls LoadScene directly instead of going through this.
    DispatchEvent<SceneLoadRequestedEvent>(e, [this](const auto& ev) {
        LoadScene(ev.path);
        return true;
        });
}

void SceneLayer::Render(graphics::ICommandList& cmd, float aspectRatio) {
    if (m_activeCamera == entt::null) return;
    m_pSceneRenderer->Render(cmd, *m_pScene, m_activeCamera, aspectRatio, m_colorFormat, m_depthFormat);
}

void SceneLayer::RenderPathTraced(graphics::ICommandList& cmd) {
    if (m_activeCamera == entt::null) return;
    // Output currently goes nowhere visible -- PathTracer writes into its
    // own internal storage texture (see PathTracer::GetOutputTexture), and
    // nothing samples/blits/displays it yet. Calling this every frame for
    // now anyway: it's the simplest way to actually exercise the shader
    // compile -> pipeline creation -> BLAS/TLAS build -> TraceRays path
    // end to end and find out if any of it is broken, rather than leaving
    // it uncalled until the display side exists too. Revisit once there's
    // a render-mode switch (see class comment) so this doesn't run
    // unconditionally forever.
    m_pPathTracer->Render(cmd, *m_pScene, m_activeCamera);
}

void SceneLayer::OnImGuiRender() {
    DrawOutlinerPanel();
    DrawInspectorPanel();
    DrawContentBrowserPanel();
}

void SceneLayer::DrawOutlinerPanel() {
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Scene");

    auto& registry = m_pScene->GetRegistry();

    // Recursive lambda needs std::function -- a plain auto lambda can't
    // reference itself by name for the recursive call.
    std::function<void(entt::entity)> drawNode = [&](entt::entity e) {
        if (!registry.valid(e)) return;

        std::string label = "Entity";
        if (auto* name = registry.try_get<Name>(e)) label = name->value;

        Hierarchy* h = registry.try_get<Hierarchy>(e);
        bool hasChildren = h && !h->children.empty();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (!hasChildren) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (m_selectedEntity == e) flags |= ImGuiTreeNodeFlags_Selected;

        bool opened = ImGui::TreeNodeEx(EntityImGuiId(e), flags, "%s", label.c_str());
        if (ImGui::IsItemClicked()) m_selectedEntity = e;

        if (hasChildren && opened) {
            for (entt::entity child : h->children) drawNode(child);
            ImGui::TreePop();
        }
        };

    // Roots = entities with a Transform but no parent -- same definition
    // Scene::UpdateWorldTransforms uses.
    for (auto entity : registry.view<Transform>()) {
        Hierarchy* h = registry.try_get<Hierarchy>(entity);
        if (!h || h->parent == entt::null) drawNode(entity);
    }

    ImGui::End();
}

void SceneLayer::DrawInspectorPanel() {
    ImGui::SetNextWindowPos(ImVec2(320, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector");

    auto& registry = m_pScene->GetRegistry();
    if (m_selectedEntity == entt::null || !registry.valid(m_selectedEntity)) {
        ImGui::TextDisabled("No entity selected");
        ImGui::End();
        return;
    }

    if (auto* name = registry.try_get<Name>(m_selectedEntity)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%s", name->value.c_str());
        if (ImGui::InputText("Name", buf, sizeof(buf))) name->value = buf;
    }

    if (auto* xf = registry.try_get<Transform>(m_selectedEntity)) {
        ImGui::DragFloat3("Position", &xf->position.x, 0.05f);

        // Edited as Euler angles for a usable widget -- round-trips
        // through glm::eulerAngles/glm::quat(vec3) lossily near
        // gimbal-lock orientations, same caveat as any Euler-angle
        // rotation widget. Fine for inspecting imported content, which
        // rarely sits exactly there.
        glm::vec3 euler = glm::degrees(glm::eulerAngles(xf->rotation));
        if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
            xf->rotation = glm::quat(glm::radians(euler));
        }

        ImGui::DragFloat3("Scale", &xf->scale.x, 0.05f, 0.001f, 1000.0f);
    }

    if (auto* cam = registry.try_get<Camera>(m_selectedEntity)) {
        ImGui::Separator();
        ImGui::Text("Camera");
        float fovDeg = glm::degrees(cam->fovYRadians);
        if (ImGui::DragFloat("FOV (Y)", &fovDeg, 0.5f, 1.0f, 179.0f)) {
            cam->fovYRadians = glm::radians(fovDeg);
        }
        ImGui::DragFloat("Near", &cam->nearPlane, 0.001f, 0.0001f, cam->farPlane - 0.001f);
        ImGui::DragFloat("Far", &cam->farPlane, 1.0f, cam->nearPlane + 0.001f, 100000.0f);
        ImGui::Checkbox("Primary", &cam->isPrimary);
    }

    if (auto* mesh = registry.try_get<MeshRenderer>(m_selectedEntity)) {
        ImGui::Separator();
        ImGui::Text("Mesh Renderer");
        if (mesh->mesh.IsValid()) {
            auto& meshAsset = m_pAssets->GetMesh(mesh->mesh);
            ImGui::Text("Vertices: %zu", meshAsset.vertices.size());
            ImGui::Text("Triangles: %zu", meshAsset.indices.size() / 3);
        }
        else {
            ImGui::TextDisabled("No mesh assigned");
        }
        // Material property editing (base color, texture) isn't wired up
        // yet -- SceneRenderer doesn't bind material data to the shader at
        // all currently (mesh.hlsl is a placeholder headlight lambert), so
        // there's nothing an edit here could actually affect.
        ImGui::TextDisabled(mesh->material.IsValid() ? "Material assigned (unused by the shader yet)"
            : "No material (default shading)");
    }

    if (auto* light = registry.try_get<Light>(m_selectedEntity)) {
        ImGui::Separator();
        ImGui::Text("Light");
        int type = static_cast<int>(light->type);
        const char* typeNames[] = { "Point", "Directional" };
        if (ImGui::Combo("Type", &type, typeNames, 2)) light->type = static_cast<Light::Type>(type);
        ImGui::ColorEdit3("Color", &light->color.x);
        ImGui::DragFloat("Intensity", &light->intensity, 0.05f, 0.0f, 1000.0f);
    }

    ImGui::End();
}

void SceneLayer::DrawContentBrowserPanel() {
    ImGui::SetNextWindowPos(ImVec2(20, 440), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620, 160), ImGuiCond_FirstUseEver);
    ImGui::Begin("Content Browser");

    ImGui::Text("Loaded scene: %s", m_pScene->name.empty() ? "(none)" : m_pScene->name.c_str());
    if (!m_pScene->sourcePath.empty()) {
        ImGui::TextDisabled("%s", m_pScene->sourcePath.string().c_str());
    }

    // NOTE: captured datasets and training jobs (see the earlier
    // content-browser design sketch) aren't real things yet -- no capture
    // pipeline or training-job bridge exists in the engine. This panel
    // currently only exercises "load a scene".
    ImGui::Separator();
    ImGui::InputText("glTF path", m_loadPathBuffer, sizeof(m_loadPathBuffer));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        LoadScene(m_loadPathBuffer); // direct call -- see the class comment on why this doesn't go through an event
    }

    ImGui::End();
}

} // namespace spray