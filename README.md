# AndUEProber

| Game | Version | GUObjectArray | DecryptFName | ProcessEventIdx |
|------|---------|----------|--------------------|-----------------|
| com.tencent.tmgp.dfm | ✅ | ✅ | ✅ | ✅ |
| com.tencent.tmgp.nz | ✅ | ✅ | ✅ | ✅ |
| com.tencent.nrc | ✅ | ✅ | ✅ | ✅ |
| com.tencent.mf.uam | ✅ | ✅ | ✅ | ✅ |
| com.tencent.tmgp.codev | ✅ | ✅ | ✅ | ✅ |
| com.tencent.ig | ✅ | ✅ | ✅ | ✅ |
| com.tencent.tmgp.pubgmhd | ✅ | ✅ | ✅ | ❌️ |

| 探测结果总览 | Dump 结果 |
|:---:|:---:|
| ![results](misc/p2.jpg) | ![dump](misc/p3.jpg) |

探测原理: [ReverseUE.md](source/UEProber/UECore/ReverseUE.md)（文档部分内容可能未及时更新）

---

## 概述

集成了 [AndUEDumper](https://github.com/MJx0/AndUEDumper)，
UEProber 可与 AndUEDumper 无缝衔接 —— 探测完成后可直接触发 Dump，无需手动配置偏移，
生成的 SDK 可直接通过 `#include "SDK_A/SDK.hpp"` 引入编译使用。

## 构建

**环境要求：**
- CMake 3.22.1+
- Android NDK（ARM64-v8a，API 27+）

```bash
# 设置 NDK_HOME 环境变量（替换为你的 NDK 实际路径）
export NDK_HOME=/path/to/android-ndk
git submodule update --init --recursive
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

> **⚠️ 请使用 Release 构建，否则注入后可能崩溃**

输出：`libAndUEProber.so`

## 使用方法

使用 [AndKittyInjector v5.1.0](https://github.com/MJx0/AndKittyInjector) 注入到目标应用：

```bash
./AndKittyInjector --package <包名> --libs libAndUEProber.so --memfd --hide --watch
```
---

## Todo

- [x] Auto-detect GUObjectArray / FName::ToString / ProcessEventIdx
- [x] Fix FName::ToString on UE 4.18
- [ ] Fix com.tencent.tmgp.pubgmhd ProcessEventIndex
- [ ] UE6
- [ ] Memory leak on FName::ToString

## Credits

- [AndUEDumper](https://github.com/MJx0/AndUEDumper)
- [AndKittyInjector](https://github.com/MJx0/AndKittyInjector)
- [Dobby](https://github.com/jmpews/Dobby)
- [AndSwapChainHook](https://github.com/DumpA1n/AndSwapChainHook)

