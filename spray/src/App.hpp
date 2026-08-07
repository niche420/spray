#pragma once

namespace spray::graphics {
class IContext;
}

namespace spray::ui {
class UIManager;
}

namespace spray
{
class Window;

class App {
public:
	App();
	~App();

	int32_t Run();

private:
	std::unique_ptr<Window> m_pWnd;
	std::unique_ptr<graphics::IContext> m_pCtx;
	std::unique_ptr<ui::UIManager> m_pUI;
};
}