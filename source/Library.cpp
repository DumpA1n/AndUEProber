#include <chrono>
#include <cstdint>
#include <thread>

#include "AndroidPlatform/AndroidPlatform.h"
#include "AndroidPlatform/LooperDispatcher.h"
#include "ImGui/ImGuiHost.h"
#include "InputEvent/InputEventHook.h"
#include "UEProber/DumperBridge.h"
#include "UEProber/UEProber.h"
#include "Utils/Config/Config.h"
#include "Utils/CrashHandler/CrashHandler.h"
#include "Utils/ElfScanner/ElfScannerManager.h"
#include "Utils/HookUtils.h"
#include "Utils/KittyEx.h"
#include "Utils/Logger.h"

#include "imgui/backends/imgui_impl_android.h"

static LooperDispatcher g_MainLooperDispatcher;

void main_thread()
{
	KT::Init();

	if (!Elf.Scan({
			"libc.so",
			"libUE4.so",
			"libvulkan.so",
			"libinput.so",
			"libart.so", // For GetJavaVM()
			"libandroid_runtime.so",
		}))
	{
		LOGE("Failed to scan necessary libraries.");
		MAKE_CRASH();
	}

	LOGI("Waiting for valid android_app* via JNI...");

	if (std::string(getprogname()).starts_with("com.tencent"))
	{
		std::thread([]()
		{
			while (true)
			{
				for (const auto& bss : Elf.UE4().bssSegments())
					mprotect((void*)bss.startAddress, bss.length, PROT_READ | PROT_WRITE);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}).detach();
	}

	GetLogFile("Debug")->Append("Hello\n"); // Must after g_App is valid

	// Config::AutoLoadOnStartup();

	ImGuiHost::Init({
		.mode         = (CFG.InjectionMode == 0) ? InjectionMode::SwapHook : InjectionMode::Overlay,
		.preferredApi = static_cast<GraphicsAPI>(CFG.RenderBackend),
		.render = []()
		{
			UEProber::GetInstance().Draw();
		},
		.postToMainThread = [](std::function<void()> task)
		{
			g_MainLooperDispatcher.post(std::move(task));
		},
	});
	g_MainLooperDispatcher.post([]() { g_MainLooperDispatcher.cleanup(); });

	// AUTO-DUMP: temporary scaffold to validate reflection-emit refactor end-to-end
	// without touching the in-game ImGui UI (DFM filters injected touch input).
	// Remove once UI tap path is solved or after S5/S6 ships.
	std::thread([]()
	{
		std::this_thread::sleep_for(std::chrono::seconds(8));
		LOGI("[AutoDump] === BEGIN ===");
		auto& prober = UEProber::GetInstance();
		prober.RunAutoDumpFlow();
		for (int i = 0; i < 120; ++i) {
			auto st = prober.GetDumpStatus();
			if (st == EDumpStatus::Success) {
				LOGI("[AutoDump] === SUCCESS === out=%s", prober.GetDumpOutputDir().c_str());
				return;
			}
			if (st == EDumpStatus::Failed) {
				LOGE("[AutoDump] === FAILED === err=%s", prober.GetDumpError().c_str());
				return;
			}
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		LOGE("[AutoDump] TIMEOUT after 120s (status still Running)");
	}).detach();

	InputEventHook::Initialize([](AInputEvent* event)
	{
		if (!event) return;

		if (ImGuiHost::IsInitialized())
		{
			ImGui_ImplAndroid_HandleInputEvent(event);
		}

        int32_t event_type = AInputEvent_getType(event);
        if (event_type == AINPUT_EVENT_TYPE_KEY)
        {
            int32_t event_key_code = AKeyEvent_getKeyCode(event);
            int32_t event_action = AKeyEvent_getAction(event);
            if (event_key_code == AKEYCODE_VOLUME_DOWN && event_action == AKEY_EVENT_ACTION_DOWN)
            {
                LOGI("keycode: AKEYCODE_VOLUME_DOWN, action: AKEY_EVENT_ACTION_DOWN");
            }
            else if (event_key_code == AKEYCODE_VOLUME_UP && event_action == AKEY_EVENT_ACTION_DOWN)
            {
                LOGI("keycode: AKEYCODE_VOLUME_UP, action: AKEY_EVENT_ACTION_DOWN");
            }
        }
    });
}

static std::atomic<bool> g_Initialized{false};

extern "C" jint JNIEXPORT JNI_OnLoad(JavaVM* vm, void* key)
{
	// key 1337 is passed by injector
	if (key != (void*)1337)
		return JNI_VERSION_1_6;

	LOGI("JNI_OnLoad called by injector.");

	LOGI("JavaVM: %p", vm);

	JNIEnv* env = nullptr;
	if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) == JNI_OK)
	{
		LOGI("JavaEnv: %p", env);
	}

	if (!g_Initialized.exchange(true))
		std::thread(main_thread).detach();

	return JNI_VERSION_1_6;
}

__attribute__((constructor)) void ctor()
{
	LOGI("ctor");






}

__attribute__((destructor)) void dtor() { LOGI("dtor"); }

#include "AndroidPlatform/android_native_app_glue.h"
extern "C" void android_main(struct android_app* /*state*/) { app_dummy(); }
