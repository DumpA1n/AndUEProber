# AndUEProber

[中文](#中文) | [English](#english)

### Screenshots

| 探测结果总览 / Detection Results | 导出结构体 / Exported Structs | Dump 结果 / Dump Result |
|:---:|:---:|:---:|
| ![results](misc/p2.jpg) | ![export](misc/p1.jpg) | ![dump](misc/p3.jpg) |

导出结构体示例 / Exported structs example: [ReverseProber_04-06_01-26-58.log](misc/ReverseProber_04-06_01-26-58.log)

探测原理 / How it works: [ReverseUE.md](source/UEProber/UECore/ReverseUE.md)（文档部分内容可能未及时更新 / Some parts may be outdated）

---

<a id="中文"></a>

### 重大更新

集成了 [AndUEDumper](https://github.com/MJx0/AndUEDumper)，UEProber 可与 AndUEDumper 无缝衔接——探测完成后可直接触发 Dump，无需手动配置偏移。

### 构建

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

### 使用方法

使用 [AndKittyInjector v5.1.0](https://github.com/MJx0/AndKittyInjector) 注入到目标应用：

```bash
./AndKittyInjector --package <包名> -lib libAndUEProber.so --memfd --hide --watch --delay 50
```

### 已验证游戏

| 游戏 | 版本 | GObjects | GetPlainANSIString | ProcessEventIdx |
|------|------|----------|--------------------|-----------------|
| com.tencent.tmgp.dfm | v1.201.37110.44 | `0x1A36A748` | Manual | ✅ |
| com.tencent.tmgp.nz | ✅ | ✅ | ✅ | ✅ |
| com.tencent.nrc | ✅ | ✅ | ✅ | ✅ |

### Todo

- [x] 自动获取 GUObjectArray / GetPlainANSIString / ProcessEventIdx

---

<a id="english"></a>

### Major Update

Integrated [AndUEDumper](https://github.com/MJx0/AndUEDumper) — UEProber now works seamlessly with AndUEDumper. After probing completes, you can trigger a Dump directly without manual offset configuration.

### Build

**Requirements:**
- CMake 3.22.1+
- Android NDK (ARM64-v8a, API 27+)

```bash
# Set NDK_HOME environment variable (replace with your actual NDK path)
export NDK_HOME=/path/to/android-ndk
git submodule update --init --recursive
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

> **⚠️ Use Release build, otherwise injection may crash**

Output: `libAndUEProber.so`

### Usage

Inject into a target app using [AndKittyInjector v5.1.0](https://github.com/MJx0/AndKittyInjector):

```bash
./AndKittyInjector --package <package_name> -lib libAndUEProber.so --memfd --hide --watch --delay 50
```

### Verified Games

| Game | Version | GUObjectArray | GetPlainANSIString | ProcessEventIdx |
|------|---------|----------|--------------------|-----------------|
| com.tencent.tmgp.dfm | v1.201.37110.44 | `0x1A36A748` | Manual | ✅ |
| com.tencent.tmgp.nz | ✅ | ✅ | ✅ | ✅ |
| com.tencent.nrc | ✅ | ✅ | ✅ | ✅ |

### Todo

- [x] Auto-detect GUObjectArray / GetPlainANSIString / ProcessEventIdx

## Credits

- [AndUEDumper](https://github.com/MJx0/AndUEDumper)
- [AndKittyInjector](https://github.com/MJx0/AndKittyInjector)
- [Dobby](https://github.com/jmpews/Dobby)
- [AndSwapChainHook](https://github.com/DumpA1n/AndSwapChainHook)

