#pragma once

struct ImGuiContext;
class SDL_Window;

namespace spray::ui {
class UIManager {
public:
    UIManager(SDL_Window* wnd);
    ~UIManager();

    void BeginFrame();
    void EndFrame();

private:
    ImGuiContext* m_ctx;
};
} // namespace spray::ui