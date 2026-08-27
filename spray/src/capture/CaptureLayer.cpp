#include "pch.hpp"
#include "CaptureLayer.hpp"
#include "scene/SceneLayer.hpp"
#include "scene/Components.hpp"
#include "graphics/CommandList.hpp"
#include "graphics/TextureReadback.hpp"
#include "graphics/Pathtracer.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/quaternion.hpp>
#include <glm/gtc/constants.hpp>

#include <stb_image_write.h>

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace spray {

namespace {

// Evenly distributes `count` points over a unit sphere using the golden-
// angle spiral (a.k.a. Fibonacci sphere) -- far more uniform coverage than
// a naive lat/long grid (which clusters samples at the poles), and doesn't
// need a separate elevation/azimuth count to configure.
std::vector<glm::vec3> FibonacciSphere(uint32_t count) {
    std::vector<glm::vec3> points;
    points.reserve(count);
    const float goldenAngle = glm::pi<float>() * (3.0f - std::sqrt(5.0f));
    for (uint32_t i = 0; i < count; ++i) {
        float t = count > 1 ? static_cast<float>(i) / static_cast<float>(count - 1) : 0.0f;
        float y = 1.0f - 2.0f * t; // top (+1) to bottom (-1)
        float radiusAtY = std::sqrt(std::max(0.0f, 1.0f - y * y));
        float theta = goldenAngle * static_cast<float>(i);
        float x = std::cos(theta) * radiusAtY;
        float z = std::sin(theta) * radiusAtY;
        points.emplace_back(x, y, z);
    }
    return points;
}

} // namespace

CaptureLayer::CaptureLayer(graphics::IDevice& device, SceneLayer& sceneLayer)
    : Layer("Capture"), m_device(device), m_sceneLayer(sceneLayer) {}

void CaptureLayer::OnImGuiRender() {
    DrawCapturePanel();
}

void CaptureLayer::DrawCapturePanel() {
    ImGui::SetNextWindowPos(ImVec2(650, 130), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 220), ImGuiCond_FirstUseEver);
    ImGui::Begin("Capture");

    ImGui::TextDisabled("Orbits the ground-truth path tracer around the");
    ImGui::TextDisabled("loaded scene and writes a posed dataset to disk.");
    ImGui::Separator();

    int viewCount = static_cast<int>(m_settings.viewCount);
    if (ImGui::DragInt("View count", &viewCount, 1.0f, 1, 512)) {
        m_settings.viewCount = static_cast<uint32_t>(std::max(1, viewCount));
    }

    int resolution = static_cast<int>(m_settings.resolution);
    if (ImGui::DragInt("Resolution", &resolution, 8.0f, 64, 4096)) {
        m_settings.resolution = static_cast<uint32_t>(std::max(64, resolution));
    }

    ImGui::DragFloat("Orbit radius x", &m_settings.orbitRadiusMultiplier, 0.05f, 1.05f, 10.0f);
    ImGui::InputText("Output dir", m_settings.outputDir, sizeof(m_settings.outputDir));

    ImGui::Separator();
    if (ImGui::Button("Capture", ImVec2(-1, 0))) {
        RunCapture();
    }

    if (!m_statusMessage.empty()) {
        ImVec4 color = m_lastCaptureFailed ? ImVec4(0.9f, 0.3f, 0.3f, 1.0f) : ImVec4(0.4f, 0.9f, 0.4f, 1.0f);
        ImGui::TextColored(color, "%s", m_statusMessage.c_str());
    }

    ImGui::End();
}

void CaptureLayer::RunCapture() {
    Scene& scene = m_sceneLayer.GetScene();
    assets::AssetManager& assets = m_sceneLayer.GetAssetManager();
    graphics::PathTracer& pathTracer = m_sceneLayer.GetPathTracer();

    scene.UpdateWorldTransforms(); // ensure bounds reflect any just-applied inspector edits
    AABB bounds = scene.ComputeWorldBounds(assets);
    if (!bounds.IsValid()) {
        m_statusMessage = "Capture failed: scene has no visible geometry to bound";
        m_lastCaptureFailed = true;
        return;
    }

    glm::vec3 center = bounds.Center();
    // Extents() is half-extents of the AABB, not a true bounding-sphere
    // radius -- close enough to size an orbit around, and avoids a
    // separate min-enclosing-sphere computation for what's just a camera
    // placement heuristic.
    float radius = glm::length(bounds.Extents());
    if (radius < 0.0001f) radius = 1.0f; // degenerate (single-point) scene -- avoid a zero-radius orbit
    float orbitRadius = radius * m_settings.orbitRadiusMultiplier;

    // Copy FOV/near/far from whichever camera is currently active in the
    // interactive viewport, so a capture roughly matches what the user was
    // just framing -- position/rotation are overwritten per-pose below
    // regardless.
    float fovY = glm::radians(60.0f);
    float nearPlane = 0.01f, farPlane = 1000.0f;
    entt::entity activeCam = m_sceneLayer.GetActiveCameraEntity();
    auto& registry = scene.GetRegistry();
    if (activeCam != entt::null && registry.valid(activeCam)) {
        if (auto* cam = registry.try_get<Camera>(activeCam)) {
            fovY = cam->fovYRadians;
            nearPlane = cam->nearPlane;
            farPlane = cam->farPlane;
        }
    }

    std::filesystem::path outDir = m_settings.outputDir;
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        m_statusMessage = "Capture failed: couldn't create output dir '" + outDir.string() + "': " + ec.message();
        m_lastCaptureFailed = true;
        return;
    }

    // Temporary capture camera -- not part of the persistent scene, torn
    // down at the end of this function regardless of success/failure.
    entt::entity captureCam = scene.CreateEntity("CaptureCamera");
    registry.emplace<Camera>(captureCam, fovY, nearPlane, farPlane, /*isPrimary=*/false);

    pathTracer.SetOutputSize(m_settings.resolution, m_settings.resolution);

    std::vector<glm::vec3> directions = FibonacciSphere(m_settings.viewCount);
    std::ostringstream posesJson;
    posesJson << "{\n  \"orbit_radius\": " << orbitRadius << ",\n  \"fov_y_radians\": " << fovY
              << ",\n  \"near\": " << nearPlane << ",\n  \"far\": " << farPlane
              << ",\n  \"resolution\": " << m_settings.resolution << ",\n  \"views\": [\n";

    uint32_t written = 0;
    for (uint32_t i = 0; i < directions.size(); ++i) {
        glm::vec3 pos = center + directions[i] * orbitRadius;

        // World up as the look-at reference, except within a small cone of
        // the poles where it's near-parallel to the view direction (which
        // degenerates quatLookAt) -- swap to a side axis there.
        glm::vec3 up{ 0.0f, 1.0f, 0.0f };
        glm::vec3 forward = glm::normalize(center - pos);
        if (std::abs(glm::dot(forward, up)) > 0.999f) up = glm::vec3(0.0f, 0.0f, 1.0f);

        // quatLookAt(direction, up) builds a rotation whose local -Z axis
        // (this engine's camera-forward convention -- see SceneLayer::
        // OnUpdate's `rotation * vec3(0,0,-1)`) points along `direction`.
        glm::quat rotation = glm::quatLookAt(forward, up);

        Transform& camXf = registry.get<Transform>(captureCam);
        camXf.position = pos;
        camXf.rotation = rotation;
        scene.UpdateWorldTransforms();

        graphics::ICommandList* cmd = m_device.BeginCommandList();
        pathTracer.Render(*cmd, scene, captureCam);
        graphics::FenceHandle fence = m_device.Submit(cmd);
        m_device.WaitForFence(fence);

        // RGBA32_Float, tightly packed -- see PathTracer::EnsureOutputTexture.
        auto pixels = graphics::ReadbackTexture(m_device, pathTracer.GetColorOutput(),
            m_settings.resolution, m_settings.resolution, /*bytesPerPixel=*/16);

        // Tonemap: plain clamp-to-[0,1] then *255. PathTracer's output has
        // no bounce lighting yet (see its class comment) so values rarely
        // exceed 1 in practice; a real tonemap operator can replace this
        // once that's no longer true, without touching anything else here.
        std::vector<uint8_t> ldr(static_cast<size_t>(m_settings.resolution) * m_settings.resolution * 4);
        const float* src = reinterpret_cast<const float*>(pixels.data());
        for (size_t p = 0; p < ldr.size() / 4; ++p) {
            for (int c = 0; c < 4; ++c) {
                float v = src[p * 4 + c];
                ldr[p * 4 + c] = static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }

        char filename[64];
        std::snprintf(filename, sizeof(filename), "view_%04u.png", i);
        std::filesystem::path imagePath = outDir / filename;
        int strideBytes = static_cast<int>(m_settings.resolution) * 4;
        if (!stbi_write_png(imagePath.string().c_str(), static_cast<int>(m_settings.resolution),
                             static_cast<int>(m_settings.resolution), 4, ldr.data(), strideBytes)) {
            continue; // image genuinely wasn't written -- skip this view's pose entry too
        }

        if (written > 0) posesJson << ",\n";
        posesJson << "    { \"file\": \"" << filename << "\""
                  << ", \"position\": [" << pos.x << ", " << pos.y << ", " << pos.z << "]"
                  << ", \"rotation_wxyz\": [" << rotation.w << ", " << rotation.x << ", "
                  << rotation.y << ", " << rotation.z << "] }";
        ++written;
    }

    posesJson << "\n  ]\n}\n";

    scene.DestroyEntity(captureCam);
    scene.UpdateWorldTransforms();

    std::filesystem::path posesPath = outDir / "poses.json";
    std::ofstream posesFile(posesPath, std::ios::trunc);
    if (posesFile) {
        posesFile << posesJson.str();
    }

    if (written == 0) {
        m_statusMessage = "Capture failed: no views were written (check output dir permissions)";
        m_lastCaptureFailed = true;
    } else {
        m_statusMessage = "Captured " + std::to_string(written) + " / " +
            std::to_string(m_settings.viewCount) + " views to '" + outDir.string() + "'";
        m_lastCaptureFailed = (written < m_settings.viewCount);
    }
}

} // namespace spray