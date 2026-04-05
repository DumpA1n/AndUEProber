#pragma once

#include "imgui/imgui.h"

// ---------------------------------------------------------------------------
// ImGuiSoftKeyboard — ImGui 内置软键盘
//
// 当 ImGui 需要文本输入时（io.WantTextInput == true），自动渲染一个
// 全功能 QWERTY 键盘面板，直接通过 io.AddInputCharacter() 注入字符。
// 完全不依赖 Android IME 框架，适用于注入/overlay 场景。
// ---------------------------------------------------------------------------

namespace ImGuiSoftKeyboard
{

/// 每帧在 NewFrame() 之后、用户 UI 之前调用：拦截键盘区域的鼠标事件
void PreUpdate();

/// 每帧在用户 UI 之后、Render() 之前调用：渲染键盘 + 处理按键
void Draw();

/// 强制显示/隐藏（覆盖自动行为）
void ForceShow(bool show);

/// 是否当前可见
bool IsVisible();

} // namespace ImGuiSoftKeyboard
