#pragma once

#include <functional>
#include <EGL/egl.h>

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

// ---------------------------------------------------------------------------
// SwapChainHook — 通过 hook 游戏的渲染提交函数注入 ImGui
//
// 原理：
//   拦截 eglSwapBuffers（OpenGL ES）或 vkQueuePresentKHR（Vulkan），
//   在游戏帧提交前将 ImGui 绘制到游戏自身的帧缓冲区。
//
// 优势（相比独立 Overlay 窗口）：
//   - 不创建额外 SurfaceView / ANativeWindow
//   - 不增加 EGL Context / Vulkan Instance
//   - 不产生多余 GPU fd / ION 映射 / SurfaceFlinger 图层
//   - 对 WindowManagerGlobal 枚举完全透明
//   - dumpsys SurfaceFlinger 看不到额外 layer
//
// 用法（在 main_thread 中）：
//   SwapChainHook::SetRenderCallback([]() {
//       UE_Hack::GetInstance().RenderMenu();
//       ...
//   });
//   SwapChainHook::Install();
// ---------------------------------------------------------------------------

namespace SwapChainHook
{

/// 安装 eglSwapBuffers 或 vkQueuePresentKHR 的 hook
void Install();

/// 卸载所有 hook，清理 ImGui 资源
void Uninstall();

/// 是否已完成 ImGui 初始化（首次 hook 触发后）
bool IsInitialized();

/// 设置每帧 ImGui 绘制回调（线程安全，可在 hook 安装前后调用）
void SetRenderCallback(std::function<void()> callback);

/// 获取检测到的游戏屏幕宽高（从 EGLSurface / VkSwapchain 取得）
int GetWidth();
int GetHeight();

} // namespace SwapChainHook
