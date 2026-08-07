#include "pch.hpp"
#include "UI.hpp"

#include <imgui.h>

namespace spray::ui {
UIManager::UIManager(SDL_Window* pWnd) 
	: m_ctx(nullptr) 
{
	m_ctx = ImGui::CreateContext();
	if (!m_ctx) {
		throw std::runtime_error("Failed to create ImGui context");
	}
}

UIManager::~UIManager()
{
	ImGui::DestroyContext(m_ctx);
	m_ctx = nullptr;
}
} 