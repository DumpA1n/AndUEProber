# UE 4.24 结构逆向探测流程

> 本文档描述在**完全没有任何已知偏移**的情况下，从零开始探测 UE 4.24 所有核心结构体成员偏移的完整流程。
> 每一步的输出都是一个或多个"已确定的偏移值"，供后续步骤作为前置条件使用。

## 前置条件

- 可以使用 `UObject::GObjects` 的所有工具函数
- 已获取到 GUObjectArray 的地址（TUObjectArray*），并能通过 `UObject::GObjects->GetByIndex(i)` 获取 UObject 指针
- 已获取到 FName 解析函数的地址（如 `FName::GetPlainANSIString`），能将 FName 转为字符串
- 能够安全读取进程内存
- 已知 UE 主模块的基址和 .text 段范围

## 关键不变量

以下是 UE4 引擎的固定事实，所有版本通用，可作为探测的锚点：

- `GetByIndex(0)` 返回的 UObject 是 "/Script/CoreUObject" Package（Name="CoreUObject"，Class.Name="Package"）
- `GetByIndex(1)` 返回的 UObject 是 UObject 基类的 UClass（Name="Object"，Outer 指向 GetByIndex(0)，Class.Name="Class"）
- `GetByIndex(2)` 及以后的对象在不同引擎中不确定，不可作为探测锚点
- UObject 的继承链: UClass -> UStruct -> UField -> UObject
- "Class" 的 Super 是 "Struct"，"Struct" 的 Super 是 "Field"，"Field" 的 Super 是 "Object"
- FField 是独立于 UObject 的轻量级反射基类（UE 4.25+ 引入，4.24 后期版本也有）
- VTable 始终在任何 UObject/FField 实例的偏移 0

---

## 第一阶段：UObject 基础成员

**目标：确定 UObject 的 VTable、Index、Name、Class、Outer、Flags 六个成员的偏移**

1. **UObject::VTable** — 始终在偏移 0，无需探测

2. **UObject::Index** — 通过 `GetByIndex(1)` 获取 UObject 指针，遍历该对象前 n 字节内存（建议 n=0x40），按 4 字节对齐逐一读取 uint32，找到值等于 1 的偏移即为 Index。可用 `GetByIndex(0)` 的 Index==0 交叉验证

3. **UObject::Name** — `GetByIndex(0)` 对象，遍历前 n 字节内存，按 FName 大小（8 字节）步进，在每个偏移处将内存解释为 FName 并调用 `FName::ToString()`，期望结果为 "CoreUObject"（或 `GetRawString` 结果为 "/Script/CoreUObject"）。能成功解析且匹配的偏移即为 Name

4. **UObject::Class** — `GetByIndex(0)` 对象，遍历前 n 字节内存，按 8 字节对齐找指针值（合法地址范围内），读取指针目标的 Name（利用步骤 3 确定的偏移），期望名称为 "Package"。匹配的偏移即为 Class

5. **UObject::Outer** — 通过 `GetByIndex(1)` 获取第二个对象，遍历其前 n 字节内存，按 8 字节对齐找指针值，若指针值等于 `GetByIndex(0)` 的地址，则该偏移为 Outer

6. **UObject::Flags** — `GetByIndex(0)` 对象，Flags的值为 1

**产出：** UObject 完整布局（6 个成员偏移 + sizeof(UObject)，sizeof 将在第二阶段通过 UStruct::Size 确认）

---

## 第二阶段：UField / UStruct 基础成员

**目标：确定 UStruct::MinAlignment, Size, Super, Children, ChildProperties, UField::Next 的偏移**

> **策略：最优先探测 MinAlignment 和 Size**。通过遍历 GObjects 直接找到 ExecuteUbergraph（无需依赖 Children），获得 UObject 精确大小后，所有后续探测（Super、Children、ChildProperties、UField::Next）均可用已知 Size 限定搜索范围，避免误匹配。

### UStruct::MinAlignment 和 Size（最优先探测）

1. 首先通过遍历 GObjects 找到 Name == "ExecuteUbergraph" 的 UFunction

2. **MinAlignment** — 有了 ExecuteUbergraph，即可探测。遍历 ExecuteUbergraph 和 obj[1]（UObject 基类，Name="Object"）的前 n 字节内存，按 4 字节对齐读取 int32，找到同一偏移处 ExecuteUbergraph=0x1 且 obj[1]=0x8 的位置即为 MinAlignment

3. **Size** — 在 MinAlignment 附近（±32 字节）按 4 字节对齐搜索，找到 ExecuteUbergraph=0x4 的偏移即为 Size。验证：obj[1] 在该偏移的值即为 sizeof(UObject)，应满足 >0x20、8 字节对齐。此时 sizeof(UObject) 已确定，后续所有探测均使用此值限定搜索范围

### UStruct::Super（利用已知 Size）

10. obj[0] 的 Class 是 "Package" UClass，搜索范围：sizeof(UObject) ~ sizeof(UObject)*3。找指针值等于 obj[1] 地址的偏移即为 Super

### UStruct::Children（利用已知 Size）

11. obj[1] 即 UObject 基类，搜索范围：sizeof(UObject) ~ sizeof(UObject)*3。找指针值，读取指针目标的 Name，期望为 "ExecuteUbergraph"。匹配的偏移即为 Children

### UStruct::ChildProperties（利用已知 Size）

12. 从 ExecuteUbergraph（已通过 GObjects 获取）中探测 ChildProperties。搜索范围：从 sizeof(UObject) 开始，到 sizeof(UObject)*3 为止（UStruct 成员区间）。找指针值，读取指针目标的 FField Name，期望为 "EntryPoint"

### UField::Next（利用已知 Size）

13. 通过 GObjects 找到 Class->Name == "Class" 且 Name == "KismetSystemLibrary" 的 UClass（先按 Class 过滤避免全量 Name 比较）。读取其 Children 得到第一个 UFunction，然后在 sizeof(UObject)~sizeof(UObject)+0x20 范围按 8 字节对齐找指针，读取目标 Name 为有效函数名则为 UField::Next
   - 验证：沿 Next 链遍历 KismetSystemLibrary 的函数链，链长通常 >= 10

**产出：** UStruct::MinAlignment, Size, Super, Children, ChildProperties, UField::Next 的偏移，以及 sizeof(UObject) 的精确值

---

## 第三阶段：UClass 成员

**目标：确定 UClass::CastFlags, DefaultObject 的偏移**

14. **UClass::CastFlags** — 利用 obj[0] 和 obj[1] 的已知精确值交叉验证。搜索范围：sizeof(UStruct)（从 "Struct" 元类的 Size 读取）到 sizeof(UClass)（从 "Class" 元类的 Size 读取），因为 CastFlags 是 UClass 独有成员：
    - "Class" 元类（obj[1] 的 Class）的 CastFlags = 0x29 (CASTCLASS_UField|CASTCLASS_UStruct|CASTCLASS_UClass)
    - "Package" UClass（obj[0] 的 Class）的 CastFlags = 0x0000000400000000 (CASTCLASS_UPackage)
    - 按 8 字节对齐读取 uint64，找到在 "Class" 元类上值为 0x29、在 "Package" UClass 上值为 0x0000000400000000 的偏移即为 CastFlags
    - 补充验证："Struct" UClass 的 CastFlags 应包含 CASTCLASS_UStruct(0x08) 但不包含 CASTCLASS_UClass(0x20)

15. **UClass::DefaultObject** — 先在 GObjects 中找到 Name=="Default__Object" 的 UObject，获取其地址。然后在 obj[1]（"Object" UClass）内存中搜索，搜索范围：sizeof(UStruct) ~ sizeof(UClass)（均从对应元类的 Size 读取）。按 8 字节对齐逐一读取指针，找到值等于 Default__Object 地址的偏移即为 DefaultObject（直接指针值比较，无需解析名称）

**产出：** UClass::CastFlags, DefaultObject 偏移

---

## 第四阶段：UFunction 结构探测

**前置条件：** 已知 UObject、UField、UStruct 所有基本成员偏移及 UStruct::Size 的精确值

### 获取 UFunction 锚点实例

17. 选取 5 个引擎内置函数作为探测锚点，每个函数的已知属性各有特点，组合使用可消除所有误匹配：

    | 锚点函数 | 获取方式 | NumParams | ParamSize | ReturnValueOffset | 关键 FunctionFlags |
    |---|---|---|---|---|---|
    | **ReceiveBeginPlay** | Actor Children 链遍历 | 0 | 0 | 0xFFFF | FUNC_Event\|FUNC_BlueprintEvent |
    | **ReceiveTick** | Actor Children 链遍历 | 1 | 4 | 0xFFFF | FUNC_Event\|FUNC_BlueprintEvent |
    | **IsValid** | KismetSystemLibrary Children 链遍历 | 2 | 9 | 8 | FUNC_Native\|FUNC_Static\|FUNC_Final\|FUNC_BlueprintPure |
    | **PrintString** | KismetSystemLibrary Children 链遍历 | 6 | 较大 | 0xFFFF | FUNC_Native\|FUNC_Static\|FUNC_Final\|FUNC_HasDefaults |
    | **K2_GetActorLocation** | Actor Children 链遍历 | 1 | 12或24 | 0 | FUNC_Native\|FUNC_Final\|FUNC_BlueprintPure |

    获取方式说明：
    - **Actor**：通过 GObjects 找到 Class->Name=="Class" && Name=="Actor" 的 UClass，然后沿 Children→UField::Next 链遍历，按 Name 匹配 ReceiveBeginPlay、ReceiveTick、K2_GetActorLocation
    - **KismetSystemLibrary**：Phase 2 中已缓存，同样沿 Children→UField::Next 链遍历，按 Name 匹配 IsValid、PrintString

    - **ReceiveTick** 与 ReceiveBeginPlay 标志完全相同但 NumParams/ParamSize 不同，形成完美蓝图函数对照组
    - **IsValid** 的 ParamSize=9（奇数）极为罕见，可有效排除误匹配
    - **PrintString** 的 NumParams=6 在引擎函数中极少见，且含独特的 FUNC_HasDefaults 标志
    - **K2_GetActorLocation** 的 ReturnValueOffset=0（唯一参数就是返回值）是非常独特的值

### 探测 UFunction::FunctionFlags

18. FunctionFlags 是 uint32，位于 UStruct 部分结尾之后（UFunction 独有成员）。搜索范围：sizeof(UStruct)（从 "Struct" 元类的 Size 读取）~ sizeof(UFunction)（从 "Function" 元类的 Size 读取），纯数值探测：
    - IsValid 必然包含 FUNC_Native(0x400) | FUNC_Final(0x01) | FUNC_BlueprintPure(0x10000000)
    - PrintString 必然包含 FUNC_Native(0x400) | FUNC_Final(0x01) | FUNC_HasDefaults(0x00010000)，且**不含** FUNC_BlueprintPure
    - ReceiveTick 必然包含 FUNC_Event(0x800) | FUNC_BlueprintEvent(0x08000000)，且**不含** FUNC_Native
    - 探测方法：在 5 个锚点函数上同一偏移读取 uint32，检查各自期望的标志位是否全部命中
    - 交叉验证：三类函数（Native+Pure / Native+HasDefaults / Event+BlueprintEvent）的标志位互不相同

### 探测 UFunction::NumParams 和 ParamSize

19. NumParams (uint8) 和 ParamSize (uint16) 位于 FunctionFlags 之后的区域（在 UE 4.24 源码布局中 NumParams 在 FunctionFlags 后约 +2~+6 字节）。使用四个不同值形成多点约束：
    - PrintString: NumParams=6, ParamSize=较大值
    - IsValid: NumParams=2, ParamSize=9
    - ReceiveTick: NumParams=1, ParamSize=4
    - ReceiveBeginPlay: NumParams=0, ParamSize=0
    - 探测方法：在 FunctionFlags 之后到 FunctionFlags+0x20 范围逐字节扫描：
      - 找 uint8 偏移：PrintString=6 且 IsValid=2 且 ReceiveTick=1 且 ReceiveBeginPlay=0 → NumParams（四个不同值同时命中，误匹配概率极低）
      - 找 uint16 偏移（2 字节对齐）：IsValid=9 且 ReceiveTick=4 且 ReceiveBeginPlay=0 → ParamSize（9 是奇数，几乎不会与其他 uint16 字段冲突）

### 探测 UFunction::ReturnValueOffset

20. ReturnValueOffset (uint16)，无返回值函数为 0xFFFF（MAX_uint16）。使用三种不同值精确锁定：
    - K2_GetActorLocation: ReturnValueOffset=0（唯一参数就是返回值，偏移为 0）
    - IsValid: ReturnValueOffset=8（返回值在参数缓冲区偏移 8 处）
    - PrintString: ReturnValueOffset=0xFFFF（无返回值）
    - 探测方法：在 FunctionFlags 之后到 FunctionFlags+0x20 范围按 2 字节对齐搜索
    - 排除已确认的 NumParams/ParamSize 偏移
    - 找到同一偏移在 K2_GetActorLocation=0、IsValid=8、PrintString=0xFFFF 的 uint16
    - 交叉验证：ReceiveBeginPlay 和 ReceiveTick 在同一偏移应为 0xFFFF

### 探测 UFunction::ExecFunction

21. ExecFunction 是函数指针（8 字节），搜索范围：sizeof(UStruct) ~ sizeof(UFunction)（均通过对应元类的 Size 读取），按 8 字节对齐遍历：
    - Native 函数（IsValid、PrintString、K2_GetActorLocation）：ExecFunction 落在 .text 段范围内，且三者的值**互不相同**
    - 蓝图函数（ReceiveBeginPlay、ReceiveTick）：ExecFunction 指向通用蓝图 VM 入口，两者在同一偏移的值**完全相同**
    - 探测方法：找到同一偏移满足——蓝图函数两者值相同、Native 函数三者值不同且均在 .text 段内

**产出：** UFunction 核心成员偏移：FunctionFlags, NumParams, ParamSize, ReturnValueOffset, ExecFunction

---

## 第五阶段：FField / FProperty 结构探测

**前置条件：** 已知 UStruct::ChildProperties 偏移，Phase 4 的 5 个锚点 UFunction

### 获取 FField 锚点实例

22. FField 不是 UObject 的子类，拥有独立的内存布局。利用 Phase 4 已确认的锚点 UFunction，读取各自的 ChildProperties 获取 FField 指针链作为探测锚点：

    | 锚点 UFunction | NumParams | 首个 FField 名称 | Property 类型 | 链长度 |
    |---|---|---|---|---|
    | **ReceiveBeginPlay** | 0 | nullptr（无参数） | — | 0 |
    | **ReceiveTick** | 1 | "DeltaSeconds" | FloatProperty | 1 |
    | **ExecuteUbergraph** | 1 | "EntryPoint" | IntProperty | 1 |
    | **IsValid** | 2 | 首参名 → "ReturnValue" | ObjectProperty → BoolProperty | 2 |
    | **K2_GetActorLocation** | 1 | "ReturnValue" | StructProperty (FVector) | 1 |

    预验证：ReceiveBeginPlay 的 ChildProperties 应为 nullptr（NumParams=0），确认 ChildProperties 偏移正确

### 探测 FField 成员

23. **FField::VTable** — 偏移 0，始终为虚函数表指针，无需探测

24. **FField::NamePrivate** — 优先探测，为后续步骤提供名称验证能力。取 ExecuteUbergraph 的 ChildProperties 指针（记为 `ffEntryPoint`），遍历其前 0x40 字节，按 FName 大小（8 字节）步进，将每个偏移处的内存解释为 FName 并调用 `FName::ToString()`，期望结果为 "EntryPoint"
    - 交叉验证：ReceiveTick 的 ChildProperties 首个 FField 在同一偏移应解析为 "DeltaSeconds"
    - 两个不同名称同时命中，确认偏移正确

25. **FField::Owner** — FFieldVariant 类型（指针 8 字节 + bool 标志 + 对齐填充，共 16 字节）。在 FField 前 0x30 字节内按 8 字节对齐搜索指针值（排除已确认的 NamePrivate 偏移）：
    - `ffEntryPoint` 在该偏移的值 == ExecuteUbergraph 地址
    - ReceiveTick 的 FField 在同一偏移的值 == ReceiveTick 地址
    - 两个不同 UFunction 的 FField 在同一偏移指向各自的 Owner，消除误匹配
    - 紧随指针之后的 8 字节中最低位为 bIsUObject 标志，Owner 指向 UFunction（UObject 子类）时应为 1

26. **FField::Next** — 利用 NumParams 差异形成正负验证对：
    - ExecuteUbergraph 的 `ffEntryPoint`：NumParams=1，Next 应为 nullptr
    - IsValid 的首个 FField：NumParams=2，Next 应为有效指针，读取目标地址在 Name 偏移处的 FName 应解析为 "ReturnValue"
    - 搜索 FField 中已确认偏移之外的指针值，找到同时满足上述两个条件（一个为 nullptr、一个指向可解析名称的 FField）的偏移即为 Next
    - 补充验证：沿 IsValid 首个 FField→Next 链遍历，链长应恰好等于 NumParams（2）

27. **FField::ClassPrivate** — FFieldClass* 类型。FFieldClass 无虚函数表，其偏移 0 处为 FName（类型名）。在 FField 前 0x30 字节内按 8 字节对齐搜索指针值（排除已确认的 Owner、Next 偏移）：
    - 读取候选指针指向地址的偏移 0 处的 FName，调用 FName::ToString()
    - `ffEntryPoint` 的 ClassPrivate→NamePrivate 应为 "IntProperty"
    - ReceiveTick FField 的 ClassPrivate→NamePrivate 应为 "FloatProperty"
    - 两个不同类型名同时命中，确认偏移正确
    - 补充验证：IsValid ReturnValue（Next 链第 2 个）的 ClassPrivate→NamePrivate 应为 "BoolProperty"

28. **FField::FlagsPrivate** — int32 (EObjectFlags) 类型。在 FField 的 0x08~0x40 范围按 4 字节对齐搜索（排除 VTable 前 8 字节及所有已确认的 FField 成员偏移覆盖范围：NamePrivate(8B)、Owner(16B)、Next(8B)、ClassPrivate(8B)）：
    - FlagsPrivate 值可能非零（如 `RF_Public|RF_MarkAsNative|RF_Transient = 0x45`），不能假设为 0
    - 值应仅包含有效 EObjectFlags 位（`& ~0x3FFFFFFF == 0`）且不超过 0xFFFF
    - 交叉验证：同一偏移处 `ffEntryPoint`、DeltaSeconds、IsValid 首参三个锚点的值应**一致**
    - 三个锚点同一偏移值一致，确认偏移正确

### 探测 FProperty 成员

29. FProperty 继承 FField，特有成员位于 FField 结尾之后。利用锚点 FField 实例，其 FProperty 成员值可精确预测：

    | 锚点 FProperty | 来源 UFunction | 类型名 | ArrayDim | ElementSize | Offset_Internal | PropertyFlags 特征 |
    |---|---|---|---|---|---|---|
    | EntryPoint | ExecuteUbergraph | IntProperty | 1 | 4 | 0 | CPF_Parm (0x80) |
    | DeltaSeconds | ReceiveTick | FloatProperty | 1 | 4 | 0 | CPF_Parm (0x80) |
    | IsValid 首参 | IsValid | ObjectProperty | 1 | 8 | 0 | CPF_Parm (0x80) |
    | IsValid ReturnValue | IsValid (Next链) | BoolProperty | 1 | 1 | 8 | CPF_Parm\|CPF_OutParm\|CPF_ReturnParm |
    | K2_GetActorLocation RV | K2_GetActorLocation | StructProperty | 1 | 12 | 0 | CPF_Parm\|CPF_OutParm\|CPF_ReturnParm |

    搜索范围：从 FField::NamePrivate 偏移 + 0x10 到 FField::NamePrivate 偏移 + 0x60，逐步探测：

    - **FProperty::ArrayDim** (int32) — 按 4 字节对齐搜索值为 1 的 int32。所有锚点属性的 ArrayDim 均为 1，找到在**所有锚点**同一偏移均为 1 的位置。值为 1 的 int32 可能有多个候选，需结合 ElementSize 联合确认

    - **FProperty::ElementSize** (int32) — 紧邻 ArrayDim 之后（ArrayDim 偏移 + 4）。利用三种不同值交叉验证：
      - EntryPoint = 4、DeltaSeconds = 4（int32/float 大小）
      - IsValid 首参 = 8（ObjectProperty，指针大小）
      - IsValid ReturnValue = 1（BoolProperty）
      - 三个不同值（1、4、8）同时命中，ArrayDim + ElementSize 的组合候选唯一确定

    - **FProperty::PropertyFlags** (uint64) — 在 ElementSize 之后按 8 字节对齐搜索：
      - 所有锚点属性必含 CPF_Parm (0x80)
      - IsValid ReturnValue 和 K2_GetActorLocation ReturnValue 还必含 CPF_ReturnParm (0x400) 和 CPF_OutParm (0x100)
      - EntryPoint 和 DeltaSeconds 不含 CPF_ReturnParm
      - 找到满足上述所有条件的 uint64 偏移

    - **FProperty::Offset_Internal** (int32) — 在 PropertyFlags 之后按 4 字节对齐搜索：
      - EntryPoint = 0、DeltaSeconds = 0、IsValid 首参 = 0（均为参数缓冲区首个参数，偏移 0）
      - IsValid ReturnValue = 8（在首参 Object 之后，Object 的 ElementSize = 8）
      - 0 和 8 两种不同值形成有效区分

    - **sizeof(FProperty)** — FProperty 子类的首个专有成员紧接 FProperty 结尾，该成员偏移即为 sizeof(FProperty)。从 Offset_Internal 偏移之后按 8 字节对齐搜索指针：
      - K2_GetActorLocation ReturnValue 是 FStructProperty，首专有成员 `Struct` 应指向 GObjects 中 Name=="Vector" 的 ScriptStruct
      - IsValid 首参是 FObjectPropertyBase，首专有成员 `PropertyClass` 应指向 GObjects 中 Name=="Object" 的 UClass（即 obj[1]）
      - 两个不同子类在同一偏移各自指向可验证的已知对象，该偏移即为 sizeof(FProperty)
      - 补充验证：IsValid ReturnValue 是 FBoolProperty，在同一偏移处读取 4 字节应为 FieldSize=1, ByteOffset=0, ByteMask=1, FieldMask=0xFF（合为 0xFF010001 小端序）

### FFieldClass 结构（参考）

30. FFieldClass 无虚函数表，布局通常为：
    - 偏移 0x00: Name (FName, 8 字节) — 类名，如 "IntProperty"、"StructProperty"
    - 偏移 0x08: Id (uint64) — 唯一标识
    - 偏移 0x10: CastFlags (uint64) — 类型转换标志
    - 偏移 0x18: ClassFlags (EClassFlags) — 类标志
    - 偏移 0x20: SuperClass (FFieldClass*) — 父类型指针

    该结构通过 FField::ClassPrivate 间接使用即可，一般不需要主动探测。如需验证继承关系，读取 SuperClass 指向的 FFieldClass::Name：如 "IntProperty" 的 Super 应为 "NumericProperty"，再 Super 应为通用 "Property"

**产出：** FField 完整成员布局（VTable、NamePrivate、Owner、Next、ClassPrivate、FlagsPrivate），FProperty 的 ArrayDim、ElementSize、PropertyFlags、Offset_Internal 偏移，sizeof(FProperty)

---

## 第六阶段：ProcessEvent 虚函数索引

**目标：确定 UObject::ProcessEvent 在虚函数表中的索引**

31. ProcessEvent 是 UObject 的虚函数，签名为 `void ProcessEvent(UFunction*, void* Parms)`。探测方法：
    - **方法 A（IDA 静态分析）**：在 IDA 中找到 ProcessEvent 函数的地址，在 UObject 的 VTable 中搜索该地址，计算索引
    - **方法 B（运行时 Hook）**：Hook 一个已知会被引擎频繁调用的 UFunction（如 Tick 相关函数），在 Hook 中回溯调用栈，分析调用方从 VTable 中取函数指针时使用的索引
    - **方法 C（特征扫描）**：ProcessEvent 内部通常会调用 `UFunction::Invoke`，可以在 .text 段通过特征匹配找到 ProcessEvent，再计算 VTable 索引

**产出：** ProcessEvent 的虚函数表索引

---

## 总结：探测顺序与依赖关系

```
阶段1: TUObjectArray (前置)
  └─→ UObject::Index, Name, Class, Outer, Flags, VTable
阶段2: UObject 成员 (前置)
  └─→ 遍历 GObjects 找到 ExecuteUbergraph
  └─→ UStruct::MinAlignment, Size → sizeof(UObject)
  └─→ UStruct::Super, Children (利用 sizeof(UObject) 限定范围)
  └─→ UStruct::ChildProperties (利用 sizeof(UObject) 限定范围)
  └─→ UField::Next (利用 sizeof(UObject) 限定范围)
阶段3: UClass 成员 (前置)
  └─→ UClass::CastFlags (利用 sizeof(UStruct)~sizeof(UClass) 限定范围)
  └─→ UClass::DefaultObject (利用 sizeof(UClass) 限定范围)
阶段4: UFunction 成员 (前置)
  └─→ UFunction::FunctionFlags (利用 sizeof(UStruct)~sizeof(UFunction) 限定范围)
  └─→ UFunction::NumParams, ParamSize, ReturnValueOffset
  └─→ UFunction::ExecFunction
阶段5: ChildProperties + Phase 4 锚点 UFunction (前置)
  └─→ FField 锚点收集（5 个锚点 UFunction 的 ChildProperties）
  └─→ FField::VTable, NamePrivate, Owner, Next, ClassPrivate, FlagsPrivate
  └─→ FProperty::ArrayDim, ElementSize, PropertyFlags, Offset_Internal
  └─→ sizeof(FProperty)
阶段6: 独立
  └─→ ProcessEvent VTable 索引
```
