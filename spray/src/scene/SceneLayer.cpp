#include "pch.hpp"
#include "SceneLayer.hpp"
#include "Components.hpp"
#include "asset/GltfImporter.hpp"
#include "event/Input.hpp"
#include "graphics/CommandList.hpp"
#include "ui/UIManager.hpp"

#include <imgui.h>

#include <glm/gtc/quaternion.hpp>

#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_mouse.h>

#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <stdexcept>
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

SceneLayer::SceneLayer(graphics::IDevice& device)
    : Layer("Scene"), m_device(device) {}

SceneLayer::~SceneLayer() {
    // Order matters: viewports must release their GPU resources before
    // ShaderLibrary's and AssetManager's GPU caches are invalidated (all
    // against the same still-alive m_device -- App guarantees WaitIdle
    // before layers are torn down, see App::~App). Order between
    // m_pPathTracer and m_pRasterizer themselves doesn't matter -- they
    // don't reference each other, only Scene/AssetManager/ShaderLibrary.
    // Order between ShaderLibrary and AssetManager invalidation doesn't
    // matter either -- independent caches.
    m_pPathTracer.reset();
    m_pRasterizer.reset();
    if (m_pShaderLibrary) m_pShaderLibrary->InvalidateGpuCache(m_device);
    if (m_pAssets) m_pAssets->InvalidateGpuCache(m_device);
}

void SceneLayer::OnAttach() {
    m_pAssets = std::make_unique<assets::AssetManager>();
    m_pScene = std::make_unique<Scene>();
    m_pShaderLibrary = std::make_unique<graphics::shaders::ShaderLibrary>();
    m_pRasterizer = std::make_unique<graphics::Rasterizer>(m_device, *m_pAssets, *m_pShaderLibrary);
    m_pPathTracer = std::make_unique<graphics::PathTracer>(m_device, *m_pAssets, *m_pShaderLibrary);

    // Placeholder camera until a real orbit controller exists -- WASD/QE
    // movement plus right-drag mouse-look (see OnUpdate) is enough for now.
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

        // Mouse-look while the right button is held (standard fly-camera
        // convention). Yaw/pitch are tracked separately and re-applied to
        // the quaternion fresh every frame rather than accumulating via
        // repeated quaternion multiplication, which drifts (roll creeps
        // in) in a way re-deriving from stored angles can't.
        if (Input::IsMouseButtonDown(SDL_BUTTON_RIGHT)) {
            glm::vec2 delta = Input::ConsumeMouseDelta();
            constexpr float kSensitivity = 0.0025f;
            m_cameraYaw -= delta.x * kSensitivity;
            m_cameraPitch -= delta.y * kSensitivity;
            const float kMaxPitch = glm::radians(89.0f);
            m_cameraPitch = glm::clamp(m_cameraPitch, -kMaxPitch, kMaxPitch);
        } else {
            // Drain accumulated motion so releasing/re-pressing the button
            // doesn't apply a jump from movement that happened while up.
            Input::ConsumeMouseDelta();
        }
        camXf.rotation = glm::quat(glm::vec3(m_cameraPitch, m_cameraYaw, 0.0f));

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

graphics::IViewport& SceneLayer::GetActiveViewport() {
    switch (m_activeMode) {
        case graphics::ViewportMode::Rasterized: return *m_pRasterizer;
        case graphics::ViewportMode::PathTraced: return *m_pPathTracer;
        case graphics::ViewportMode::Splat:
            // No SplatViewer exists yet -- see Viewport.hpp's comment on
            // why the enum value exists ahead of any implementation.
            // Falling through to Rasterized rather than crashing: DrawView
            // portModePanel below doesn't currently offer Splat as a
            // selectable option for exactly this reason, so reaching this
            // would mean m_activeMode was set some other way (a bug worth
            // surfacing loudly rather than silently rendering the wrong
            // thing).
            throw std::runtime_error("SceneLayer: ViewportMode::Splat has no implementation yet");
    }
    throw std::runtime_error("SceneLayer: unhandled ViewportMode");
}

void SceneLayer::RenderActiveViewport(graphics::ICommandList& cmd, uint32_t width, uint32_t height) {
    if (m_activeCamera == entt::null) return;
    graphics::IViewport& viewport = GetActiveViewport();
    viewport.SetOutputSize(width, height);
    viewport.Render(cmd, *m_pScene, m_activeCamera);
}

graphics::TextureHandle SceneLayer::GetActiveColorOutput() const {
    switch (m_activeMode) {
        case graphics::ViewportMode::Rasterized: return m_pRasterizer->GetColorOutput();
        case graphics::ViewportMode::PathTraced: return m_pPathTracer->GetColorOutput();
        case graphics::ViewportMode::Splat: return {};
    }
    return {};
}

void SceneLayer::OnImGuiRender() {
    DrawViewportPanel();
    DrawViewportModePanel();
    DrawOutlinerPanel();
    DrawInspectorPanel();
    DrawContentBrowserPanel();
}

void SceneLayer::DrawViewportPanel() {
    // Zero padding so the image fills the panel edge-to-edge rather than
    // sitting inset inside ImGui's default window padding.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Viewport");

    ImVec2 avail = ImGui::GetContentRegionAvail();
    // Floor at 1x1 -- a fully collapsed/zero-area panel would otherwise
    // feed a 0-sized request down to SetOutputSize (PathTracer divides by
    // height for aspect ratio; a literal 0 there is a crash, not a no-op).
    m_viewportPanelSize = {
        static_cast<uint32_t>(avail.x > 1.0f ? avail.x : 1.0f),
        static_cast<uint32_t>(avail.y > 1.0f ? avail.y : 1.0f),
    };

    // One frame of latency: this displays whatever RenderActiveViewport
    // rendered last frame at last frame's panel size, and the size read
    // above is what this frame's App::RenderFrame will render at for next
    // frame's display. Standard for a docked-image viewport -- imperceptible
    // in practice, and far simpler than resizing/re-rendering mid-ImGui-pass.
    graphics::TextureHandle output = GetActiveColorOutput();
    if (output.IsValid() && m_pUI) {
        ImGui::Image(m_pUI->GetTextureID(output), avail);
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

void SceneLayer::DrawViewportModePanel() {
    ImGui::SetNextWindowPos(ImVec2(650, 20), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 90), ImGuiCond_FirstUseEver);
    ImGui::Begin("Viewport Settings");

    // Splat deliberately omitted -- no implementation yet, see
    // GetActiveViewport's comment. Add it here once SplatViewer exists.
    static const char* kModeNames[] = { "Rasterized", "Path Traced" };
    int modeIndex = static_cast<int>(m_activeMode);
    if (ImGui::Combo("Mode", &modeIndex, kModeNames, 2)) {
        m_activeMode = static_cast<graphics::ViewportMode>(modeIndex);
    }

    if (m_activeMode == graphics::ViewportMode::PathTraced) {
        ImGui::TextDisabled("Ground truth -- slower, no bounce lighting yet");
    } else {
        ImGui::TextDisabled("Fast preview -- not representative of a capture");
    }

    ImGui::End();
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
        // yet -- Rasterizer doesn't bind material data to the shader at
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

    // NOTE: captured datasets and training jobs get their own Layer
    // (see CaptureLayer, a sibling of this one) rather than living here.
    // This panel only exercises "load a scene".
    ImGui::Separator();
    ImGui::InputText("glTF path", m_loadPathBuffer, sizeof(m_loadPathBuffer));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        LoadScene(m_loadPathBuffer); // direct call -- see the class comment on why this doesn't go through an event
    }

    ImGui::End();
}

} // namespace spray