#include "AndroidPlatform/AndroidApp.h"
#include "Core/ElfScannerManager.h"
#include "SwapChain/SwapChainHook.h"
#include "UEProber/UEProber.h"
#include "Utils/CrashHandler.h"
#include "Utils/FileLogger.h"
#include "Utils/HookUtils.h"
#include "Utils/Logger.h"
#include "imgui/imgui.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <sys/mman.h>
#include <thread>

void main_thread()
{
	CrashHandler::Install();

	if (!Elf.scanAsync({
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
				mprotect((void*)Elf.UE4().bss(), Elf.UE4().bssSize(), PROT_READ | PROT_WRITE);
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}).detach();
	}

	while (!(g_App = AndroidApp::FindAndroidAppViaJNI()))
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	LOGI("[*] g_App: %p", g_App);

	GetLogFile("Debug")->Append("Hello\n"); // Must after g_App is valid

	SwapChainHook::SetRenderCallback([]()
	{
		UEProber::GetInstance().Draw();
	});
	SwapChainHook::Install();
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

	// Enable if not use AndKittyInjector
	// if (!g_Initialized.exchange(true))
	// 	std::thread(main_thread).detach();
}

__attribute__((destructor)) void dtor() { LOGI("dtor"); }
