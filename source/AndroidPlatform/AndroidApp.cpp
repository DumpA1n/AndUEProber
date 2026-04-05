#include "AndroidApp.h"
#include <jni.h>
#include "Core/ElfScannerManager.h"
#include "Utils/Logger.h"

struct android_app *g_App = nullptr;

JavaVM* g_JavaVM = nullptr; /// deprecated, use AndroidApp::GetJavaVM() instead

namespace AndroidApp {

/**
 * 通过 ElfScanner 解析 libart.so 的 JNI_GetCreatedJavaVMs 符号获取 JavaVM 指针
 */
JavaVM* GetJavaVM()
{
    static JavaVM* s_vm = nullptr;
    if (s_vm) return s_vm;

    auto addr = Elf.art().findSymbol("JNI_GetCreatedJavaVMs");
    if (!addr)
    {
        LOGE("[AndroidApp] GetJavaVM: findSymbol(JNI_GetCreatedJavaVMs) failed");
        return nullptr;
    }

    using JNI_GetCreatedJavaVMs_t = jint (*)(JavaVM**, jsize, jsize*);
    auto fn = reinterpret_cast<JNI_GetCreatedJavaVMs_t>(addr);

    JavaVM* vm = nullptr;
    jsize count = 0;
    if (fn(&vm, 1, &count) == JNI_OK && count > 0)
    {
        s_vm = vm;
        LOGI("[AndroidApp] GetJavaVM: got VM=%p", s_vm);
        return s_vm;
    }

    LOGE("[AndroidApp] GetJavaVM: JNI_GetCreatedJavaVMs failed, count=%d", (int)count);
    return nullptr;
}

JNIEnv *GetJavaEnv()
{
    JavaVM* vm = GetJavaVM();
    if (!vm)
    {
        LOGE("[AndroidApp] GetJavaEnv: JavaVM is null");
        return nullptr;
    }
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_OK)
        return env;
    LOGW("[AndroidApp] GetJavaEnv: GetEnv failed (thread not attached?)");
    return nullptr;
}

/**
 * 通过 JNI 反射获取 android_app*，适用于所有使用 NativeActivity + android_native_app_glue 的 Android 应用
 *
 * 原理：
 *   ActivityThread.mActivities -> ActivityClientRecord.activity -> NativeActivity
 *   NativeActivity.mNativeHandle (long) 即 ANativeActivity*
 *   ANativeActivity::instance 由 glue code 设置为 android_app*
 */
android_app* FindAndroidAppViaJNI()
{
    JavaVM* vm = GetJavaVM();
    if (!vm)
    {
        LOGE("[AndroidApp] FindAndroidAppViaJNI: JavaVM is null");
        return nullptr;
    }

    JNIEnv* env = nullptr;
    bool needDetach = false;

    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK)
    {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        {
            LOGE("[AndroidApp] FindAndroidAppViaJNI: AttachCurrentThread failed");
            return nullptr;
        }
        needDetach = true;
    }

    android_app* result = nullptr;

    // PushLocalFrame 确保所有 JNI 局部引用在 PopLocalFrame 时自动释放
    if (env->PushLocalFrame(32) == 0)
    {
        do {
            // 1. ActivityThread.currentActivityThread()
            jclass atClass = env->FindClass("android/app/ActivityThread");
            if (!atClass || env->ExceptionCheck()) { env->ExceptionClear(); break; }

            jmethodID catMethod = env->GetStaticMethodID(atClass, "currentActivityThread",
                "()Landroid/app/ActivityThread;");
            if (!catMethod || env->ExceptionCheck()) { env->ExceptionClear(); break; }

            jobject at = env->CallStaticObjectMethod(atClass, catMethod);
            if (!at || env->ExceptionCheck()) { env->ExceptionClear(); break; }

            // 2. mActivities: ArrayMap<IBinder, ActivityClientRecord> (API 21+)
            jfieldID activitiesField = env->GetFieldID(atClass, "mActivities",
                "Landroid/util/ArrayMap;");
            if (!activitiesField || env->ExceptionCheck()) { env->ExceptionClear(); break; }

            jobject activities = env->GetObjectField(at, activitiesField);
            if (!activities || env->ExceptionCheck()) { env->ExceptionClear(); break; }

            // 3. 遍历 ArrayMap 获取 ActivityClientRecord
            jclass mapClass = env->GetObjectClass(activities);
            jmethodID sizeMethod = env->GetMethodID(mapClass, "size", "()I");
            jmethodID valueAtMethod = env->GetMethodID(mapClass, "valueAt",
                "(I)Ljava/lang/Object;");
            if (!sizeMethod || !valueAtMethod) break;

            jint size = env->CallIntMethod(activities, sizeMethod);

            // 4. 查找 NativeActivity（含子类）并读取 mNativeHandle
            jclass naClass = env->FindClass("android/app/NativeActivity");
            if (!naClass || env->ExceptionCheck()) { env->ExceptionClear(); break; }

            jfieldID handleField = env->GetFieldID(naClass, "mNativeHandle", "J");
            if (!handleField || env->ExceptionCheck()) { env->ExceptionClear(); break; }

            for (jint i = 0; i < size; i++)
            {
                jobject record = env->CallObjectMethod(activities, valueAtMethod, i);
                if (!record) continue;

                jclass recClass = env->GetObjectClass(record);
                jfieldID actField = env->GetFieldID(recClass, "activity",
                    "Landroid/app/Activity;");
                if (!actField || env->ExceptionCheck())
                {
                    env->ExceptionClear();
                    continue;
                }

                jobject activity = env->GetObjectField(record, actField);
                if (!activity) continue;

                // IsInstanceOf 会匹配 NativeActivity 及其所有子类（如 UE4 GameActivity）
                if (env->IsInstanceOf(activity, naClass))
                {
                    jlong handle = env->GetLongField(activity, handleField);
                    LOGI("[AndroidApp] FindAndroidAppViaJNI: NativeActivity found, mNativeHandle=0x%llx", (unsigned long long)handle);
                    if (handle != 0)
                    {
                        auto* na = reinterpret_cast<ANativeActivity*>(handle);
                        result = static_cast<android_app*>(na->instance);
                        LOGI("[AndroidApp] FindAndroidAppViaJNI: android_app=%p", result);
                    }
                }
                if (result) break;
            }
        } while (false);

        env->PopLocalFrame(nullptr);
    }

    if (env->ExceptionCheck())
        env->ExceptionClear();

    if (needDetach)
        vm->DetachCurrentThread();

    return result;
}

}
