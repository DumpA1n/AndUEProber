#include "SwapChainHook.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <android/native_window.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vulkan/vulkan_core.h>

#include "Core/ElfScannerManager.h"
#include "Core/ResourceManager.h"
#include "Dobby/dobby.h"
#include "ImGuiExtern/ImGuiSoftKeyboard.h"
#include "AndroidPlatform/AndroidApp.h"
#include "PointerHook/PointerHookManager.h"
#include "PointerHook/SafePointerHook.h"
#include "Utils/Logger.h"
#include "imgui/backends/imgui_impl_android.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "imgui/backends/imgui_impl_vulkan.h"
#include "imgui/imgui.h"

// ===========================================================================
// 内部状态
// ===========================================================================

// hook 策略
enum class HookStrategy { EGL, VkCreateInstance, VkGIPA_Pointer, VkGIPA_Thunk };

// EGL              — 方案1: hook eglSwapBuffers (OpenGL)
// VkCreateInstance — 方案2: hook vkCreateInstance (Vulkan)
// VkGIPA_Pointer   — 方案3: hook vkGetInstanceProcAddr 指针 (UE + Vulkan)
// VkGIPA_Thunk     — 方案4: hook vkGetInstanceProcAddr thunk (UE + Vulkan)
static constexpr HookStrategy kDefaultStrategy = HookStrategy::EGL;

static const std::unordered_map<std::string, HookStrategy> kPackageStrategies = {
    { "com.tencent.tmgp.dfm", HookStrategy::VkGIPA_Thunk },
    { "com.tencent.tmgp.nz",  HookStrategy::VkGIPA_Thunk },
    { "com.tencent.mf.uam",   HookStrategy::EGL },
    { "com.tencent.nrc",      HookStrategy::EGL },
    { "com.tencent.ig",       HookStrategy::EGL },
};

static HookStrategy ResolveHookStrategy()
{
    const char* progName = getprogname();
    if (progName)
    {
        auto it = kPackageStrategies.find(progName);
        if (it != kPackageStrategies.end())
        {
            LOGI("[SwapChainHook] Package '%s' matched strategy %d", progName, (int)it->second);
            return it->second;
        }
        LOGI("[SwapChainHook] Package '%s' not in strategy table, using default %d", progName, (int)kDefaultStrategy);
    }
    return kDefaultStrategy;
}

static const HookStrategy g_HookStrategy = ResolveHookStrategy();

static std::atomic<bool>           g_Installed{false};
static std::atomic<bool>           g_ImGuiReady{false};
static std::atomic<bool>           g_VkGIPAHooked{false}; // vkGetInstanceProcAddr Dobby hook 是否存活
static void*                       g_VkGIPAImplAddr = nullptr; // 解析出的实际实现地址
static std::mutex                  g_CallbackMutex;
static std::function<void()>       g_RenderCallback;
static int                         g_Width  = 0;
static int                         g_Height = 0;

static int32_t (*g_Orig_onInputEvent)(struct android_app* app, AInputEvent* event);

/**
 * @brief 从 ARM64 thunk 函数解析尾调用目标地址
 *
 * vkGetInstanceProcAddr 导出符号只有两条指令：
 *   BTI c          (0xD503245F)
 *   B   sub_XXXXX  (000101 | imm26)
 * 无法直接 inline hook，需解码 B 指令的立即数得到实际实现函数地址
 */
static void* ResolveArm64TailCall(void* thunkAddr)
{
    auto* code = (const uint32_t*)thunkAddr;

    // 跳过可能的 BTI 指令（编码 0xD503245F）
    int idx = 0;
    if (code[0] == 0xD503245F)
        idx = 1;

    uint32_t insn = code[idx];
    // 检查是否为 B 指令：bits[31:26] == 000101
    if ((insn >> 26) != 0b000101)
    {
        LOGE("[SwapChainHook] Not a B instruction at %p+%d: 0x%08X", thunkAddr, idx * 4, insn);
        return nullptr;
    }

    // 提取 imm26 并符号扩展
    int32_t imm26 = (int32_t)(insn & 0x03FFFFFF);
    if (imm26 & (1 << 25))  // 符号位
        imm26 |= (int32_t)0xFC000000;

    // target = PC + imm26 * 4
    uintptr_t pc = (uintptr_t)&code[idx];
    uintptr_t target = pc + ((int64_t)imm26 << 2);

    LOGI("[SwapChainHook] Resolved thunk %p -> impl %p (B offset=0x%X)",
         thunkAddr, (void*)target, (uint32_t)(imm26 << 2));
    return (void*)target;
}

// ---------------------------------------------------------------------------
// OpenGL ES 路径
// ---------------------------------------------------------------------------

// eglSwapBuffers 原始函数指针
static EGLBoolean (*g_OrigEglSwapBuffers)(EGLDisplay, EGLSurface) = nullptr;

// 标记：是否已初始化 OpenGL 路径的 ImGui
static bool g_GlesInitialized = false;

// 记录 ImGui 初始化时使用的 ANativeWindow，用于检测 window 重建
static ANativeWindow* g_ImGuiWindow = nullptr;

/**
 * @brief 在游戏的 EGL Context 上初始化 ImGui（仅首次调用）
 *
 * 复用游戏的 Context，直接渲染到默认 FBO（帧缓冲区 0），
 * 无需创建独立 EGLContext / EGLSurface。
 */
static bool InitImGuiOnGameContext(EGLDisplay display, EGLSurface surface)
{
    // 获取 Surface 尺寸
    EGLint w = 0, h = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &w);
    eglQuerySurface(display, surface, EGL_HEIGHT, &h);
    LOGI("[SwapChainHook] eglQuerySurface returned %dx%d", w, h);

    // 回退：从 ANativeWindow 获取尺寸
    if ((w <= 0 || h <= 0) && g_App && g_App->window)
    {
        w = ANativeWindow_getWidth(g_App->window);
        h = ANativeWindow_getHeight(g_App->window);
        LOGI("[SwapChainHook] Fallback to ANativeWindow size %dx%d", w, h);
    }

    if (w <= 0 || h <= 0)
    {
        LOGE("[SwapChainHook] Invalid surface size %dx%d", w, h);
        return false;
    }
    g_Width = w;
    g_Height = h;

    // 创建 ImGui 上下文
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 样式
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowBorderSize = 1.0f;
    style.WindowRounding = 0.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.ScrollbarSize = 20.0f;
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(4.0f, 4.0f);
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;
    style.ScaleAllSizes(2.0f);

    // 平台后端
    ImGui_ImplAndroid_Init(g_App->window);

    // OpenGL 渲染后端（复用游戏 Context）
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // 字体
    ResourceManager::GetInstance().initializeFonts(30.0f);

    g_GlesInitialized = true;
    g_ImGuiReady.store(true);
    g_ImGuiWindow = g_App->window;

    LOGI("[SwapChainHook] ImGui initialized on game EGL context  %dx%d", w, h);
    return true;
}

/**
 * @brief eglSwapBuffers hook
 *
 * 在游戏调用 eglSwapBuffers 提交帧之前：
 *   1. 保存完整 GL 状态
 *   2. 渲染 ImGui 到游戏的默认帧缓冲区（FBO 0）
 *   3. 恢复 GL 状态
 *   4. 调用原始 eglSwapBuffers
 */
static EGLBoolean Hooked_eglSwapBuffers(EGLDisplay display, EGLSurface surface)
{
    if (eglGetCurrentContext() == EGL_NO_CONTEXT)
        return g_OrigEglSwapBuffers(display, surface);

    if (!g_GlesInitialized)
    {
        if (!InitImGuiOnGameContext(display, surface))
        {
            // 初始化失败，直接调用原始函数
            return g_OrigEglSwapBuffers(display, surface);
        }
    }

    if (!g_App || !g_App->window)
        return g_OrigEglSwapBuffers(display, surface);

    // ANativeWindow 重建后需要重新初始化 ImGui
    if (g_GlesInitialized && g_App->window != g_ImGuiWindow)
    {
        LOGI("[SwapChainHook] ANativeWindow changed %p -> %p, reinitializing ImGui", g_ImGuiWindow, g_App->window);
        g_ImGuiReady.store(false);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
        g_GlesInitialized = false;
        g_ImGuiWindow = nullptr;
        if (!InitImGuiOnGameContext(display, surface))
            return g_OrigEglSwapBuffers(display, surface);
    }

    // --- 查询 EGL surface 实际尺寸 ---
    EGLint sw = 0, sh = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &sw);
    eglQuerySurface(display, surface, EGL_HEIGHT, &sh);

    // --- ImGui 渲染帧 ---
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();

    // 通过 DisplayFramebufferScale 计算 ImGui 渲染后端真实帧缓冲分辨率。
    {
        ImGuiIO &ioUpdate = ImGui::GetIO();
        // DisplaySize 保持 ANativeWindow 尺寸（与触摸坐标一致）
        float dispW = ioUpdate.DisplaySize.x;
        float dispH = ioUpdate.DisplaySize.y;
        g_Width  = (int)dispW;
        g_Height = (int)dispH;
        // 设置 framebuffer scale: EGL surface 尺寸 / 显示尺寸
        if (sw > 0 && sh > 0 && dispW > 0 && dispH > 0)
        {
            ioUpdate.DisplayFramebufferScale = ImVec2((float)sw / dispW, (float)sh / dispH);
        }
    }

    ImGui::NewFrame();
    ImGuiSoftKeyboard::PreUpdate();

    if (g_RenderCallback)
        g_RenderCallback();

    ImGuiSoftKeyboard::Draw();

    ImGui::Render();

    // --- 保存游戏的 GL 状态 ---
    GLint prevFBO = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
    GLint prevViewport[4];
    glGetIntegerv(GL_VIEWPORT, prevViewport);
    GLboolean prevBlend = glIsEnabled(GL_BLEND);
    GLint prevBlendSrcRGB, prevBlendDstRGB, prevBlendSrcA, prevBlendDstA;
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevBlendSrcRGB);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevBlendDstRGB);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevBlendSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevBlendDstA);

    // 渲染到游戏的默认帧缓冲区（FBO 0）
    // 注意：不做 glClear，保留游戏已有画面内容
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // viewport 使用 EGL surface 实际分辨率（framebuffer 尺寸）
    ImGuiIO &io = ImGui::GetIO();
    int fbW = (int)(io.DisplaySize.x * io.DisplayFramebufferScale.x);
    int fbH = (int)(io.DisplaySize.y * io.DisplayFramebufferScale.y);
    glViewport(0, 0, fbW, fbH);

    // 开启混合，ImGui 绘制叠加在游戏画面之上
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    // --- 恢复游戏的 GL 状态 ---
    glBindFramebuffer(GL_FRAMEBUFFER, prevFBO);
    glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
    if (prevBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    glBlendFuncSeparate(prevBlendSrcRGB, prevBlendDstRGB, prevBlendSrcA, prevBlendDstA);

    // --- 调用原始 eglSwapBuffers ---
    return g_OrigEglSwapBuffers(display, surface);
}

// ---------------------------------------------------------------------------
// Vulkan 路径
// ---------------------------------------------------------------------------

// Vulkan 函数指针
static PFN_vkCreateInstance        g_OrigVkCreateInstance    = nullptr;
static PFN_vkQueuePresentKHR       g_OrigVkQueuePresent      = nullptr;
static PFN_vkCreateDevice          g_OrigVkCreateDevice      = nullptr;
static PFN_vkDestroyDevice         g_OrigVkDestroyDevice     = nullptr;
static PFN_vkGetDeviceQueue        g_OrigVkGetDeviceQueue    = nullptr;
static PFN_vkCreateSwapchainKHR    g_OrigVkCreateSwapchain   = nullptr;
static PFN_vkDestroySwapchainKHR   g_OrigVkDestroySwapchain  = nullptr;

// 记录所有通过 DobbyHook 安装的地址，用于 Uninstall 时 DobbyDestroy
static std::mutex                  g_HookedAddrsMutex;
static std::vector<void*>          g_DobbyHookedAddrs;

// 捕获的 Vulkan 对象
static VkPhysicalDevice g_VkPhysDev     = VK_NULL_HANDLE;
static VkDevice         g_VkDevice      = VK_NULL_HANDLE;
static VkQueue          g_VkQueue       = VK_NULL_HANDLE;
static uint32_t         g_VkQueueFamily = UINT32_MAX;

// Vulkan ImGui 渲染资源
static VkRenderPass      g_VkRenderPass      = VK_NULL_HANDLE;
static VkCommandPool     g_VkCommandPool     = VK_NULL_HANDLE;
static VkDescriptorPool  g_VkDescriptorPool  = VK_NULL_HANDLE;
static VkSwapchainKHR    g_VkSwapchain       = VK_NULL_HANDLE;

static std::vector<VkImage>          g_VkSwapImages;
static std::vector<VkImageView>      g_VkSwapImageViews;
static std::vector<VkFramebuffer>    g_VkFramebuffers;
static std::vector<VkCommandBuffer>  g_VkCommandBuffers;
static std::vector<VkFence>          g_VkFences;

static VkFormat   g_VkSwapFormat = VK_FORMAT_UNDEFINED;
static VkExtent2D g_VkSwapExtent = {};  // 交换链实际尺寸（可能是竖屏）
static VkSurfaceTransformFlagBitsKHR g_VkPreTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
static bool       g_VkInitialized = false;

/**
 * @brief 判断 preTransform 是否包含 90°/270° 旋转
 */
static bool IsRotated90or270(VkSurfaceTransformFlagBitsKHR transform)
{
    return (transform & (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
                         VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) != 0;
}

/**
 * @brief 旋转 ImGui DrawData 的顶点和裁剪矩形
 *
 * 游戏使用 preTransform 旋转时，交换链图像是竖屏方向，
 * 但 ImGui 按横屏布局生成顶点。需要将横屏坐标映射到竖屏帧缓冲区。
 *
 * ROTATE_90 (90° CW):
 *   横屏 (x, y) → 竖屏 (dispH - y, x)
 *   裁剪矩形 (x1,y1,x2,y2) → (dispH - y2, x1, dispH - y1, x2)
 *
 * ROTATE_270 (270° CW = 90° CCW):
 *   横屏 (x, y) → 竖屏 (y, dispW - x)
 *   裁剪矩形 (x1,y1,x2,y2) → (y1, dispW - x2, y2, dispW - x1)
 *
 * 注意：旋转使用 DisplaySize（顶点实际坐标范围），而非 fbExtent（交换链像素）。
 * 旋转后通过 FramebufferScale 将 rotated-display 坐标映射到交换链帧缓冲像素。
 */
static void RotateImDrawData(ImDrawData *drawData,
                             VkSurfaceTransformFlagBitsKHR transform,
                             VkExtent2D fbExtent)
{
    // 顶点坐标所在的 DisplaySize 空间
    float dispW = drawData->DisplaySize.x;
    float dispH = drawData->DisplaySize.y;

    bool is90  = (transform & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)  != 0;
    bool is270 = (transform & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) != 0;

    if (!is90 && !is270)
        return;

    // 旋转所有顶点（使用 DisplaySize 坐标域）
    for (int n = 0; n < drawData->CmdListsCount; n++)
    {
        ImDrawList *cmdList = drawData->CmdLists[n];
        for (int i = 0; i < cmdList->VtxBuffer.Size; i++)
        {
            ImDrawVert &v = cmdList->VtxBuffer[i];
            float x = v.pos.x;
            float y = v.pos.y;
            if (is90)
            {
                v.pos.x = dispH - y;
                v.pos.y = x;
            }
            else // is270
            {
                v.pos.x = y;
                v.pos.y = dispW - x;
            }
        }

        // 旋转裁剪/剪裁矩形
        for (int i = 0; i < cmdList->CmdBuffer.Size; i++)
        {
            ImDrawCmd &cmd = cmdList->CmdBuffer[i];
            float cx1 = cmd.ClipRect.x;
            float cy1 = cmd.ClipRect.y;
            float cx2 = cmd.ClipRect.z;
            float cy2 = cmd.ClipRect.w;
            if (is90)
            {
                cmd.ClipRect = ImVec4(dispH - cy2, cx1, dispH - cy1, cx2);
            }
            else // is270
            {
                cmd.ClipRect = ImVec4(cy1, dispW - cx2, cy2, dispW - cx1);
            }
        }
    }

    // 旋转后：顶点坐标范围从 (dispW × dispH) 变为 (dispH × dispW)
    // 通过 FramebufferScale 将 rotated-display 坐标映射到实际交换链帧缓冲像素
    float rotW = dispH;  // 旋转后的显示宽度
    float rotH = dispW;  // 旋转后的显示高度
    float fbW  = (float)fbExtent.width;
    float fbH  = (float)fbExtent.height;

    drawData->DisplaySize      = ImVec2(rotW, rotH);
    drawData->DisplayPos       = ImVec2(0, 0);
    drawData->FramebufferScale = ImVec2(fbW / rotW, fbH / rotH);
}

/**
 * @brief 清理 Vulkan ImGui 渲染资源
 */
static void CleanupVkResources()
{
    if (g_VkDevice == VK_NULL_HANDLE)
        return;

    vkDeviceWaitIdle(g_VkDevice);

    if (g_VkInitialized)
        ImGui_ImplVulkan_Shutdown();

    for (auto f : g_VkFences)
        if (f) vkDestroyFence(g_VkDevice, f, nullptr);
    g_VkFences.clear();

    g_VkCommandBuffers.clear();

    for (auto fb : g_VkFramebuffers)
        if (fb) vkDestroyFramebuffer(g_VkDevice, fb, nullptr);
    g_VkFramebuffers.clear();

    for (auto iv : g_VkSwapImageViews)
        if (iv) vkDestroyImageView(g_VkDevice, iv, nullptr);
    g_VkSwapImageViews.clear();

    g_VkSwapImages.clear();

    if (g_VkRenderPass)    { vkDestroyRenderPass(g_VkDevice, g_VkRenderPass, nullptr);       g_VkRenderPass    = VK_NULL_HANDLE; }
    if (g_VkCommandPool)   { vkDestroyCommandPool(g_VkDevice, g_VkCommandPool, nullptr);     g_VkCommandPool   = VK_NULL_HANDLE; }
    if (g_VkDescriptorPool){ vkDestroyDescriptorPool(g_VkDevice, g_VkDescriptorPool, nullptr);g_VkDescriptorPool= VK_NULL_HANDLE; }

    g_VkInitialized = false;
}

/**
 * @brief 为 Vulkan 交换链创建 ImGui 资源（RenderPass、Framebuffer 等）
 *
 * RenderPass 使用 LOAD_OP_LOAD 保留游戏原有画面，ImGui 叠加渲染其上。
 */
static bool CreateVkImGuiResources()
{
    if (g_VkDevice == VK_NULL_HANDLE || g_VkSwapImages.empty())
        return false;

    // -- RenderPass: 保留游戏画面（LOAD_OP_LOAD），ImGui 叠加 --
    VkAttachmentDescription attachment{};
    attachment.format         = g_VkSwapFormat;
    attachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;     // 保留游戏帧
    attachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout  = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    VkSubpassDependency dep{};
    dep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass    = 0;
    dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &attachment;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies   = &dep;

    if (vkCreateRenderPass(g_VkDevice, &rpInfo, nullptr, &g_VkRenderPass) != VK_SUCCESS)
    {
        LOGE("[SwapChainHook/Vk] vkCreateRenderPass failed");
        return false;
    }

    // -- ImageView + Framebuffer --
    uint32_t imageCount = (uint32_t)g_VkSwapImages.size();
    g_VkSwapImageViews.resize(imageCount);
    g_VkFramebuffers.resize(imageCount);

    for (uint32_t i = 0; i < imageCount; i++)
    {
        VkImageViewCreateInfo ivInfo{};
        ivInfo.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivInfo.image    = g_VkSwapImages[i];
        ivInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivInfo.format   = g_VkSwapFormat;
        ivInfo.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ivInfo.subresourceRange.baseMipLevel   = 0;
        ivInfo.subresourceRange.levelCount     = 1;
        ivInfo.subresourceRange.baseArrayLayer = 0;
        ivInfo.subresourceRange.layerCount     = 1;

        if (vkCreateImageView(g_VkDevice, &ivInfo, nullptr, &g_VkSwapImageViews[i]) != VK_SUCCESS)
        {
            LOGE("[SwapChainHook/Vk] vkCreateImageView[%u] failed", i);
            return false;
        }

        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass      = g_VkRenderPass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments    = &g_VkSwapImageViews[i];
        fbInfo.width           = g_VkSwapExtent.width;
        fbInfo.height          = g_VkSwapExtent.height;
        fbInfo.layers          = 1;

        if (vkCreateFramebuffer(g_VkDevice, &fbInfo, nullptr, &g_VkFramebuffers[i]) != VK_SUCCESS)
        {
            LOGE("[SwapChainHook/Vk] vkCreateFramebuffer[%u] failed", i);
            return false;
        }
    }

    // -- CommandPool + CommandBuffers --
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = g_VkQueueFamily;

    if (vkCreateCommandPool(g_VkDevice, &poolInfo, nullptr, &g_VkCommandPool) != VK_SUCCESS)
    {
        LOGE("[SwapChainHook/Vk] vkCreateCommandPool failed");
        return false;
    }

    g_VkCommandBuffers.resize(imageCount);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool        = g_VkCommandPool;
    allocInfo.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = imageCount;

    if (vkAllocateCommandBuffers(g_VkDevice, &allocInfo, g_VkCommandBuffers.data()) != VK_SUCCESS)
    {
        LOGE("[SwapChainHook/Vk] vkAllocateCommandBuffers failed");
        return false;
    }

    // -- Fence（每张图像一个，用于等待上一次该 cmd 完成） --
    g_VkFences.resize(imageCount);
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (uint32_t i = 0; i < imageCount; i++)
    {
        if (vkCreateFence(g_VkDevice, &fenceInfo, nullptr, &g_VkFences[i]) != VK_SUCCESS)
        {
            LOGE("[SwapChainHook/Vk] vkCreateFence[%u] failed", i);
            return false;
        }
    }

    // -- DescriptorPool（ImGui 字体纹理等） --
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 },
    };
    VkDescriptorPoolCreateInfo dpInfo{};
    dpInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpInfo.maxSets       = 16;
    dpInfo.poolSizeCount = 1;
    dpInfo.pPoolSizes    = poolSizes;

    if (vkCreateDescriptorPool(g_VkDevice, &dpInfo, nullptr, &g_VkDescriptorPool) != VK_SUCCESS)
    {
        LOGE("[SwapChainHook/Vk] vkCreateDescriptorPool failed");
        return false;
    }

    LOGI("[SwapChainHook/Vk] Resources created  images=%u  %ux%u",
         imageCount, g_VkSwapExtent.width, g_VkSwapExtent.height);
    return true;
}

/**
 * @brief 初始化 ImGui Vulkan 后端
 */
static bool InitImGuiVulkan()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // 确定逻辑显示尺寸（始终为横屏）
    // 当 preTransform 包含 90°/270° 旋转时，imageExtent 是竖屏尺寸，需要交换
    uint32_t displayW = g_VkSwapExtent.width;
    uint32_t displayH = g_VkSwapExtent.height;
    if (IsRotated90or270(g_VkPreTransform))
    {
        std::swap(displayW, displayH);
        LOGI("[SwapChainHook/Vk] preTransform=0x%x, swapped display %ux%u → %ux%u",
             (int)g_VkPreTransform, g_VkSwapExtent.width, g_VkSwapExtent.height, displayW, displayH);
    }

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)displayW, (float)displayH);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowBorderSize = 1.0f;
    style.WindowRounding = 0.0f;
    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
    style.ScrollbarSize = 20.0f;
    style.FramePadding = ImVec2(8.0f, 6.0f);
    style.TouchExtraPadding = ImVec2(4.0f, 4.0f);
    style.AntiAliasedLines = true;
    style.AntiAliasedFill = true;
    style.ScaleAllSizes(2.0f);

    // 平台后端
    ImGui_ImplAndroid_Init(g_App->window);

    // Vulkan 渲染后端
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance        = VK_NULL_HANDLE; // 不需要，ImGui 不自行创建资源
    initInfo.PhysicalDevice  = g_VkPhysDev;
    initInfo.Device          = g_VkDevice;
    initInfo.QueueFamily     = g_VkQueueFamily;
    initInfo.Queue           = g_VkQueue;
    initInfo.DescriptorPool  = g_VkDescriptorPool;
    initInfo.RenderPass      = g_VkRenderPass;
    initInfo.MinImageCount   = (uint32_t)g_VkSwapImages.size();
    initInfo.ImageCount      = (uint32_t)g_VkSwapImages.size();
    initInfo.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    initInfo.CheckVkResultFn = [](VkResult r) {
        if (r != VK_SUCCESS) LOGE("[SwapChainHook/Vk] ImGui VkResult=%d", (int)r);
    };

    if (!ImGui_ImplVulkan_Init(&initInfo))
    {
        LOGE("[SwapChainHook/Vk] ImGui_ImplVulkan_Init failed");
        return false;
    }

    // 字体
    ResourceManager::GetInstance().initializeFonts(30.0f);

    // 对外报告横屏尺寸
    g_Width = (int)displayW;
    g_Height = (int)displayH;
    g_VkInitialized = true;
    g_ImGuiReady.store(true);

    LOGI("[SwapChainHook/Vk] ImGui initialized  display=%ux%u  fb=%ux%u  preTransform=0x%x",
         displayW, displayH, g_VkSwapExtent.width, g_VkSwapExtent.height, (int)g_VkPreTransform);
    return true;
}

// ---------------------------------------------------------------------------
// Vulkan Hooks
// ---------------------------------------------------------------------------

/**
 * @brief vkCreateDevice hook — 捕获 VkPhysicalDevice / VkDevice / 队列族
 */
static VkResult VKAPI_CALL Hooked_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDevice *pDevice)
{
    VkResult result = g_OrigVkCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result == VK_SUCCESS && pDevice)
    {
        g_VkPhysDev = physicalDevice;
        g_VkDevice  = *pDevice;

        // 从 DeviceCreateInfo 获取图形队列族索引
        if (pCreateInfo && pCreateInfo->queueCreateInfoCount > 0)
        {
            g_VkQueueFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
        }

        LOGI("[SwapChainHook/Vk] Captured device=%p  physDev=%p  queueFamily=%u",
             g_VkDevice, g_VkPhysDev, g_VkQueueFamily);
    }
    return result;
}

/**
 * @brief vkGetDeviceQueue hook — 捕获图形队列
 */
static void VKAPI_CALL Hooked_vkGetDeviceQueue(
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue)
{
    g_OrigVkGetDeviceQueue(device, queueFamilyIndex, queueIndex, pQueue);
    if (pQueue && *pQueue && queueFamilyIndex == g_VkQueueFamily)
    {
        g_VkQueue = *pQueue;
        LOGI("[SwapChainHook/Vk] Captured queue=%p  family=%u", g_VkQueue, queueFamilyIndex);
    }
}

/**
 * @brief vkCreateSwapchainKHR hook — 捕获交换链信息并创建渲染资源
 */
static VkResult VKAPI_CALL Hooked_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain)
{
    // 先清理旧资源（交换链重建时）
    if (g_VkInitialized)
    {
        CleanupVkResources();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
    }

    VkResult result = g_OrigVkCreateSwapchain(device, pCreateInfo, pAllocator, pSwapchain);
    if (result != VK_SUCCESS || !pSwapchain)
        return result;

    g_VkSwapchain    = *pSwapchain;
    g_VkSwapFormat   = pCreateInfo->imageFormat;
    g_VkSwapExtent   = pCreateInfo->imageExtent;
    g_VkPreTransform = (VkSurfaceTransformFlagBitsKHR)pCreateInfo->preTransform;

    // 获取交换链图像
    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, nullptr);
    g_VkSwapImages.resize(imageCount);
    vkGetSwapchainImagesKHR(device, *pSwapchain, &imageCount, g_VkSwapImages.data());

    LOGI("[SwapChainHook/Vk] Swapchain created  %ux%u  images=%u  format=%d",
         g_VkSwapExtent.width, g_VkSwapExtent.height, imageCount, (int)g_VkSwapFormat);

    // 创建 ImGui 渲染资源
    if (g_VkDevice != VK_NULL_HANDLE && g_VkQueue != VK_NULL_HANDLE)
    {
        if (CreateVkImGuiResources())
            InitImGuiVulkan();
    }

    return result;
}

/**
 * @brief vkDestroySwapchainKHR hook — 清理渲染资源
 */
static void VKAPI_CALL Hooked_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
    if (swapchain == g_VkSwapchain && g_VkInitialized)
    {
        CleanupVkResources();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
        g_ImGuiReady.store(false);
    }
    g_OrigVkDestroySwapchain(device, swapchain, pAllocator);
}

/**
 * @brief vkDestroyDevice hook — 完全清理
 */
static void VKAPI_CALL Hooked_vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks *pAllocator)
{
    if (device == g_VkDevice)
    {
        CleanupVkResources();
        if (ImGui::GetCurrentContext())
        {
            ImGui_ImplAndroid_Shutdown();
            ImGui::DestroyContext();
        }
        g_ImGuiReady.store(false);
        g_VkDevice = VK_NULL_HANDLE;
        g_VkQueue  = VK_NULL_HANDLE;
    }
    g_OrigVkDestroyDevice(device, pAllocator);
}

/**
 * @brief vkQueuePresentKHR hook — ImGui 渲染注入点
 *
 * 在游戏调用 present 前：
 *   1. 等待该图像对应的 fence
 *   2. 录制 ImGui 渲染命令到命令缓冲
 *   3. 提交命令缓冲（等待游戏的 renderFinished 信号量）
 *   4. 调用原始 vkQueuePresentKHR
 */
static VkResult VKAPI_CALL Hooked_vkQueuePresentKHR(
    VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    if (!g_VkInitialized || !pPresentInfo || pPresentInfo->swapchainCount == 0)
        return g_OrigVkQueuePresent(queue, pPresentInfo);

    // 找到我们跟踪的交换链
    for (uint32_t sc = 0; sc < pPresentInfo->swapchainCount; sc++)
    {
        if (pPresentInfo->pSwapchains[sc] != g_VkSwapchain)
            continue;

        uint32_t imageIndex = pPresentInfo->pImageIndices[sc];
        if (imageIndex >= g_VkCommandBuffers.size())
            continue;

        // 等待上一次使用该命令缓冲的 GPU 工作完成
        vkWaitForFences(g_VkDevice, 1, &g_VkFences[imageIndex], VK_TRUE, UINT64_MAX);
        vkResetFences(g_VkDevice, 1, &g_VkFences[imageIndex]);

        // --- ImGui 新帧 ---
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplAndroid_NewFrame();

        {
            ImGuiIO &io = ImGui::GetIO();
            g_Width  = (int)io.DisplaySize.x;
            g_Height = (int)io.DisplaySize.y;
        }

        ImGui::NewFrame();
        ImGuiSoftKeyboard::PreUpdate();

        if (g_RenderCallback)
            g_RenderCallback();

        ImGuiSoftKeyboard::Draw();

        ImGui::Render();

        // --- 旋转 ImGui 绘制数据以匹配帧缓冲区方向 ---
        if (IsRotated90or270(g_VkPreTransform))
            RotateImDrawData(ImGui::GetDrawData(), g_VkPreTransform, g_VkSwapExtent);

        // --- 录制命令缓冲 ---
        VkCommandBuffer cmd = g_VkCommandBuffers[imageIndex];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass        = g_VkRenderPass;
        rpBegin.framebuffer       = g_VkFramebuffers[imageIndex];
        rpBegin.renderArea.extent = g_VkSwapExtent;

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        // --- 提交：等待游戏的 present 信号量 ---
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo{};
        submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers    = &cmd;

        // 如果游戏有 wait semaphore，我们也等待它
        // 这确保游戏渲染完成后再叠加 ImGui
        if (pPresentInfo->waitSemaphoreCount > 0 && pPresentInfo->pWaitSemaphores)
        {
            submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
            submitInfo.pWaitSemaphores    = pPresentInfo->pWaitSemaphores;
            // 为每个 wait semaphore 提供一个 wait stage
            std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, waitStage);
            submitInfo.pWaitDstStageMask  = waitStages.data();

            vkQueueSubmit(queue, 1, &submitInfo, g_VkFences[imageIndex]);

            // 重要：清除 present 的 wait semaphore，因为我们已经消费了它们
            // 需要修改 PresentInfo（const_cast 安全，因为仅本帧使用）
            auto *mutablePresent = const_cast<VkPresentInfoKHR*>(pPresentInfo);
            mutablePresent->waitSemaphoreCount = 0;
            mutablePresent->pWaitSemaphores    = nullptr;
        }
        else
        {
            vkQueueSubmit(queue, 1, &submitInfo, g_VkFences[imageIndex]);
        }

        break; // 仅处理第一个匹配的交换链
    }

    return g_OrigVkQueuePresent(queue, pPresentInfo);
}


// ===========================================================================
// InlineHook方案 (通用方案，适用于所有程序)
// ===========================================================================

/**
 * @brief 在 vkCreateInstance 成功后，通过 vkGetInstanceProcAddr 解析并 hook 其余 Vulkan 函数
 */
static void HookVulkanFunctionsFromInstance(VkInstance instance)
{
    // 只 hook 一次，避免重复 DobbyHook 同一地址
    static std::atomic<bool> s_Hooked{false};
    if (s_Hooked.exchange(true))
        return;

    struct VkHookEntry {
        const char* name;
        void*       hookFunc;
        void**      origSlot;
    };

    VkHookEntry hooks[] = {
        { "vkCreateDevice",        (void*)Hooked_vkCreateDevice,        (void**)&g_OrigVkCreateDevice      },
        { "vkDestroyDevice",       (void*)Hooked_vkDestroyDevice,       (void**)&g_OrigVkDestroyDevice     },
        { "vkGetDeviceQueue",      (void*)Hooked_vkGetDeviceQueue,      (void**)&g_OrigVkGetDeviceQueue    },
        { "vkCreateSwapchainKHR",  (void*)Hooked_vkCreateSwapchainKHR,  (void**)&g_OrigVkCreateSwapchain   },
        { "vkDestroySwapchainKHR", (void*)Hooked_vkDestroySwapchainKHR, (void**)&g_OrigVkDestroySwapchain  },
        { "vkQueuePresentKHR",     (void*)Hooked_vkQueuePresentKHR,     (void**)&g_OrigVkQueuePresent      },
    };

    for (auto& h : hooks)
    {
        void* sym = (void*)vkGetInstanceProcAddr(instance, h.name);
        if (sym)
        {
            DobbyHook(sym, h.hookFunc, h.origSlot);
            {
                std::lock_guard<std::mutex> lk(g_HookedAddrsMutex);
                g_DobbyHookedAddrs.push_back(sym);
            }
            LOGI("[SwapChainHook] %s hooked  addr=%p", h.name, sym);
        }
        else
        {
            LOGE("[SwapChainHook] %s not found via vkGetInstanceProcAddr", h.name);
        }
    }
}

/**
 * @brief vkCreateInstance hook — 捕获 VkInstance 后解析并 hook 其余函数
 */
static VkResult VKAPI_CALL Hooked_vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkInstance *pInstance)
{
    VkResult result = g_OrigVkCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (result == VK_SUCCESS && pInstance && *pInstance)
    {
        LOGI("[SwapChainHook/Vk] Captured VkInstance=%p", *pInstance);
        // 第二阶段：用有效的 VkInstance 解析并 hook 其余 Vulkan 函数
        HookVulkanFunctionsFromInstance(*pInstance);
    }
    return result;
}


// ===========================================================================
// InlineHook + 指针Hook方案 (适用于使用 vkGetInstanceProcAddr 获取函数指针的程序)
// ===========================================================================

install_hook_name(vkGetInstanceProcAddr, PFN_vkVoidFunction, VkInstance instance, const char* pName)
{
    if (!pName)
        return orig_vkGetInstanceProcAddr(instance, pName);

    struct HookEntry { void* hookFunc; void** origSlot; };
    static const std::unordered_map<std::string, HookEntry> kHooks = {
        { "vkCreateDevice",        { (void*)Hooked_vkCreateDevice,        (void**)&g_OrigVkCreateDevice      } },
        { "vkDestroyDevice",       { (void*)Hooked_vkDestroyDevice,       (void**)&g_OrigVkDestroyDevice     } },
        { "vkGetDeviceQueue",      { (void*)Hooked_vkGetDeviceQueue,      (void**)&g_OrigVkGetDeviceQueue    } },
        { "vkCreateSwapchainKHR",  { (void*)Hooked_vkCreateSwapchainKHR,  (void**)&g_OrigVkCreateSwapchain   } },
        { "vkDestroySwapchainKHR", { (void*)Hooked_vkDestroySwapchainKHR, (void**)&g_OrigVkDestroySwapchain  } },
        { "vkQueuePresentKHR",     { (void*)Hooked_vkQueuePresentKHR,     (void**)&g_OrigVkQueuePresent      } },
    };
    static std::atomic<size_t> s_HookedCount{0};

    auto it = kHooks.find(pName);
    if (it == kHooks.end())
        return orig_vkGetInstanceProcAddr(instance, pName);

    const auto& e = it->second;
    auto origFunc = orig_vkGetInstanceProcAddr(instance, pName);
    if (origFunc && *e.origSlot == nullptr)
    {
        *e.origSlot = (void*)origFunc;
        size_t count = s_HookedCount.fetch_add(1) + 1;

        LOGI("[PointerHook] vkGetInstanceProcAddr  %s → orig=%p  hook=%p  (%zu/%zu)",
             pName, (void*)origFunc, e.hookFunc, count, kHooks.size());

        if (count >= kHooks.size() && g_VkGIPAHooked.exchange(false))
        {
            LOGI("[PointerHook] All %zu hooks captured, destroying vkGetInstanceProcAddr Dobby hook", kHooks.size());
            DobbyDestroy(g_VkGIPAImplAddr);
        }
    }
    else
    {
        LOGI("[PointerHook] vkGetInstanceProcAddr  %s → orig=%p  hook=%p  (already captured)",
             pName, (void*)origFunc, e.hookFunc);
    }

    return (PFN_vkVoidFunction)e.hookFunc;
}


// ===========================================================================
// 指针Hook方案
// ===========================================================================

class vkGetInstanceProcAddrHook : public SafePointerHook
{
public:
    vkGetInstanceProcAddrHook() : SafePointerHook() {}
    ~vkGetInstanceProcAddrHook() override = default;

    std::string GetName() const override { return "vkGetInstanceProcAddrHook"; }

    uintptr_t FakeFunction(RegContext* ctx) override
    {
        // VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(
        //     VkInstance                                  instance,
        //     const char*                                 pName);

        VkInstance instance = (VkInstance)ctx->general.x[0];
        const char* pName = (const char*)ctx->general.x[1];

        if (!pName)
            return GetOrigFuncAddr();

        struct HookEntry { void* hookFunc; void** origSlot; };
        static const std::unordered_map<std::string, HookEntry> kHooks = {
            { "vkCreateDevice",        { (void*)Hooked_vkCreateDevice,        (void**)&g_OrigVkCreateDevice      } },
            { "vkDestroyDevice",       { (void*)Hooked_vkDestroyDevice,       (void**)&g_OrigVkDestroyDevice     } },
            { "vkGetDeviceQueue",      { (void*)Hooked_vkGetDeviceQueue,      (void**)&g_OrigVkGetDeviceQueue    } },
            { "vkCreateSwapchainKHR",  { (void*)Hooked_vkCreateSwapchainKHR,  (void**)&g_OrigVkCreateSwapchain   } },
            { "vkDestroySwapchainKHR", { (void*)Hooked_vkDestroySwapchainKHR, (void**)&g_OrigVkDestroySwapchain  } },
            { "vkQueuePresentKHR",     { (void*)Hooked_vkQueuePresentKHR,     (void**)&g_OrigVkQueuePresent      } },
        };
        static std::atomic<size_t> s_HookedCount{0};

        auto it = kHooks.find(pName);
        if (it == kHooks.end())
            return GetOrigFuncAddr();

        const auto& e = it->second;
        auto origFunc = orig_vkGetInstanceProcAddr(instance, pName);
        if (origFunc && *e.origSlot == nullptr)
        {
            *e.origSlot = (void*)origFunc;
            size_t count = s_HookedCount.fetch_add(1) + 1;

            LOGI("[PointerHook] vkGetInstanceProcAddr  %s → orig=%p  hook=%p  (%zu/%zu)",
                pName, (void*)origFunc, e.hookFunc, count, kHooks.size());

            if (count >= kHooks.size() && g_VkGIPAHooked.exchange(false))
            {
                LOGI("[PointerHook] All %zu hooks captured, destroying vkGetInstanceProcAddr Dobby hook", kHooks.size());
                RestoreHook();
            }
        }
        else
        {
            LOGI("[PointerHook] vkGetInstanceProcAddr  %s → orig=%p  hook=%p  (already captured)",
                pName, (void*)origFunc, e.hookFunc);
        }

        ctx->general.x[0] = (uintptr_t)e.hookFunc;
        return 0;
    }

protected:
    uintptr_t GetElfBaseImpl() const override { return Elf.UE4().base(); }
    uintptr_t GetPtrAddrImpl() const override { return GetElfBaseImpl() + 0x1A2B7E68; }
    uintptr_t GetFuncAddrImpl() const override { return (uintptr_t)&vkGetInstanceProcAddr; }
};



// ===========================================================================
// 公开接口
// ===========================================================================

namespace SwapChainHook
{

void Install()
{
    if (g_Installed.exchange(true))
        return;

    LOGI("[SwapChainHook] Installing hooks...  strategy=%d", (int)g_HookStrategy);

    switch (g_HookStrategy)
    {
    case HookStrategy::EGL:
    {
        // --- 方案1: EGL hook (适用所有 OpenGL 后端程序) ---
        void* sym = (void*)&eglSwapBuffers;
        DobbyHook(sym, (void *)Hooked_eglSwapBuffers, (void **)&g_OrigEglSwapBuffers);
        break;
    }
    case HookStrategy::VkCreateInstance:
    {
        // --- 方案2: Vulkan InlineHook (适用所有 vulkan 后端程序) ---
        void* sym = (void*)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
        if (sym)
        {
            DobbyHook(sym, (void*)Hooked_vkCreateInstance, (void**)&g_OrigVkCreateInstance);
            g_DobbyHookedAddrs.push_back(sym);
            LOGI("[SwapChainHook] vkCreateInstance hooked  addr=%p", sym);
        }
        else
        {
            LOGE("[SwapChainHook] vkCreateInstance not found via vkGetInstanceProcAddr");
        }
        break;
    }
    case HookStrategy::VkGIPA_Pointer:
    {
        // --- 方案3: vkGetInstanceProcAddr 指针 hook（适用 UE + vulkan 程序) ---
        // uintptr_t ptrAddr = Elf.UE4().base() + 0x1A2B7E68;
        // while (*(uintptr_t*)ptrAddr == 0) {
        //     std::this_thread::sleep_for(std::chrono::milliseconds(1));
        // }
        // LOGI("[SwapChainHook] vkGetInstanceProcAddr GOT value=%p", (void*)*(uintptr_t*)ptrAddr);
        // PointerHookManager::GetInstance().Add<vkGetInstanceProcAddrHook>();
        break;
    }
    case HookStrategy::VkGIPA_Thunk:
    {
        // --- 方案4: vkGetInstanceProcAddr InlineHook (适用 UE + vulkan 程序) ---
        g_VkGIPAImplAddr = ResolveArm64TailCall((void*)vkGetInstanceProcAddr);
        if (g_VkGIPAImplAddr)
        {
            install_hook_vkGetInstanceProcAddr(g_VkGIPAImplAddr);
            g_VkGIPAHooked.store(true);
        }
        else
        {
            LOGE("[SwapChainHook] Failed to resolve vkGetInstanceProcAddr impl");
        }
        break;
    }
    }

    if (g_App)
    {
        g_Orig_onInputEvent = g_App->onInputEvent;
        g_App->onInputEvent = [](struct android_app* app, AInputEvent* event) -> int32_t {
            if (g_ImGuiReady.load() && ImGui::GetCurrentContext())
                ImGui_ImplAndroid_HandleInputEvent(event);
            return g_Orig_onInputEvent(app, event);
        };
        LOGI("[SwapChainHook] Android input event hooked");
    }

    LOGI("[SwapChainHook] Hooks installed");
}

void Uninstall()
{
    if (!g_Installed.exchange(false))
        return;

    LOGI("[SwapChainHook] Uninstalling...");

    switch (g_HookStrategy)
    {
    case HookStrategy::EGL:
    {
        if (g_OrigEglSwapBuffers)
        {
            DobbyDestroy((void*)&eglSwapBuffers);
            g_OrigEglSwapBuffers = nullptr;
        }
        break;
    }
    case HookStrategy::VkCreateInstance:
    {
        std::lock_guard<std::mutex> lk(g_HookedAddrsMutex);
        for (void* addr : g_DobbyHookedAddrs)
            DobbyDestroy(addr);
        g_DobbyHookedAddrs.clear();
        break;
    }
    case HookStrategy::VkGIPA_Pointer:
    {
        PointerHookManager::GetInstance().Remove<vkGetInstanceProcAddrHook>();
        break;
    }
    case HookStrategy::VkGIPA_Thunk:
    {
        if (g_VkGIPAHooked.exchange(false) && g_VkGIPAImplAddr)
            DobbyDestroy(g_VkGIPAImplAddr);
        break;
    }
    }

    // 清理 ImGui
    if (g_VkInitialized)
        CleanupVkResources();

    if (ImGui::GetCurrentContext())
    {
        if (g_GlesInitialized)
            ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplAndroid_Shutdown();
        ImGui::DestroyContext();
    }

    g_GlesInitialized = false;
    g_VkInitialized   = false;
    g_ImGuiReady.store(false);

    if (g_App)
        g_App->onInputEvent = g_Orig_onInputEvent;

    LOGI("[SwapChainHook] Uninstalled");
}

bool IsInitialized()
{
    return g_ImGuiReady.load();
}

void SetRenderCallback(std::function<void()> callback)
{
    std::lock_guard<std::mutex> lk(g_CallbackMutex);
    g_RenderCallback = std::move(callback);
}

int GetWidth()  { return g_Width; }
int GetHeight() { return g_Height; }

} // namespace SwapChainHook
