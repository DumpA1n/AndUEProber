#pragma once

#include "Basic.h"
#include "UnrealContainers.h"
#include <cstddef>

namespace SDK {

class UObject {
public:
	~UObject() {}

	static inline class TUObjectArrayWrapper      GObjects;

    void**                                            VTable;
    class UClass*                                     Class;
    class UObject*                                    Outer;
    uint32                                            Flags;
    FName                                             Name;
    uint32                                            Index;

public:
	static class UObject* FindObjectFastImpl(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None);
	static class UObject* FindObjectImpl(const std::string& FullName, EClassCastFlags RequiredType = EClassCastFlags::None);

    std::string GetName() const;
    std::string GetFullName() const;
	bool HasTypeFlag(EClassCastFlags TypeFlags) const;
	bool IsA(EClassCastFlags TypeFlags) const;
	bool IsA(UClass* cmp) const;
    void ProcessEvent(void* fn, void* parms) const;
	bool IsDefaultObject() const;

	void ExecuteUbergraph(int32 EntryPoint);

	void TraverseSupers(const std::function<bool(const UObject*)>& Callback) const;

public:
	static class UClass* FindClass(const std::string& ClassFullName)
	{
		return FindObject<class UClass>(ClassFullName, EClassCastFlags::Class);
	}
	static class UClass* FindClassFast(const std::string& ClassName)
	{
		return FindObjectFast<class UClass>(ClassName, EClassCastFlags::Class);
	}
	
	template<typename UEType = UObject>
	static UEType* FindObject(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None)
	{
		return static_cast<UEType*>(FindObjectImpl(Name, RequiredType));
	}
	template<typename UEType = UObject>
	static UEType* FindObjectFast(const std::string& Name, EClassCastFlags RequiredType = EClassCastFlags::None)
	{
		return static_cast<UEType*>(FindObjectFastImpl(Name, RequiredType));
	}

	void ProcessEvent(class UFunction* Function, void* Parms) const
	{
		InSDKUtils::CallGameFunction(InSDKUtils::GetVirtualFunction<void(*)(const UObject*, class UFunction*, void*)>(this, Offsets::ProcessEventIdx), this, Function, Parms);
	}
};
static_assert(alignof(UObject) == 0x000008, "Wrong alignment on UObject");
static_assert(sizeof(UObject) == 0x000028, "Wrong size on UObject");
static_assert(offsetof(UObject, VTable) == 0x000000, "Member 'UObject::VTable' has a wrong offset!");
static_assert(offsetof(UObject, Class) == 0x000008, "Member 'UObject::Class' has a wrong offset!");
static_assert(offsetof(UObject, Outer) == 0x000010, "Member 'UObject::Outer' has a wrong offset!");
static_assert(offsetof(UObject, Flags) == 0x000018, "Member 'UObject::Flags' has a wrong offset!");
static_assert(offsetof(UObject, Name) == 0x00001C, "Member 'UObject::Name' has a wrong offset!");
static_assert(offsetof(UObject, Index) == 0x000024, "Member 'UObject::Index' has a wrong offset!");

class UField : public UObject {
public:
	class UField*                                 Next;                                              // 0x0030(0x0008)(NOT AUTO-GENERATED PROPERTY)

public:
	static class UClass* StaticClass()
	{
		return StaticClassImpl<"Field">();
	}
	static class UField* GetDefaultObj()
	{
		return GetDefaultObjImpl<UField>();
	}
};
static_assert(alignof(UField) == 0x000008, "Wrong alignment on UField");
static_assert(sizeof(UField) == 0x000030, "Wrong size on UField");
static_assert(offsetof(UField, Next) == 0x000028, "Member 'UField::Next' has a wrong offset!");

class UStruct : public UField {
public:
	uint8                                         Pad_30[0xC];                                       // 0x0030(0x000C)(Fixing Size After Last Property [ Dumper-7 ])
	int32                                         PropertiesSize;                                    // 0x003C(0x0004)(NOT AUTO-GENERATED PROPERTY)
	class UStruct*                                Super;                                             // 0x0040(0x0008)(NOT AUTO-GENERATED PROPERTY)
	int32                                         MinAlignment;                                      // 0x0048(0x0004)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_4C[0x4];                                       // 0x004C(0x0004)(Fixing Size After Last Property [ Dumper-7 ])
	class UField*                                 Children;                                          // 0x0050(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_58[0x10];                                      // 0x0058(0x0010)(Fixing Size After Last Property [ Dumper-7 ])
	class FField*                                 ChildProperties;                                   // 0x0068(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_70[0x40];                                      // 0x0070(0x0040)(Fixing Struct Size After Last Property [ Dumper-7 ])

public:
	bool IsSubclassOf(const UStruct* Base) const;

public:
	static class UClass* StaticClass()
	{
		return StaticClassImpl<"Struct">();
	}
	static class UStruct* GetDefaultObj()
	{
		return GetDefaultObjImpl<UStruct>();
	}
};
static_assert(alignof(UStruct) == 0x000008, "Wrong alignment on UStruct");
static_assert(sizeof(UStruct) == 0x0000B0, "Wrong size on UStruct");
static_assert(offsetof(UStruct, PropertiesSize) == 0x00003C, "Member 'UStruct::PropertiesSize' has a wrong offset!");
static_assert(offsetof(UStruct, Super) == 0x000040, "Member 'UStruct::Super' has a wrong offset!");
static_assert(offsetof(UStruct, MinAlignment) == 0x000048, "Member 'UStruct::MinAlignment' has a wrong offset!");
static_assert(offsetof(UStruct, Children) == 0x000050, "Member 'UStruct::Children' has a wrong offset!");
static_assert(offsetof(UStruct, ChildProperties) == 0x000068, "Member 'UStruct::ChildProperties' has a wrong offset!");

class UFunction : public UStruct
{
public:
	using FNativeFuncPtr = void (*)(void* Context, void* TheStack, void* Result);

	uint8                                         NumParms;                                          // 0x00B0(0x0001)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_B1[0x1];                                       // 0x00B1(0x0001)(Fixing Size After Last Property [ Dumper-7 ])
	uint16                                        ParmsSize;                                         // 0x00B2(0x0002)(NOT AUTO-GENERATED PROPERTY)
	uint16                                        ReturnValueOffset;                                 // 0x00B4(0x0002)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_B6[0x2];                                       // 0x00B6(0x0002)(Fixing Size After Last Property [ Dumper-7 ])
	EFunctionFlags                                FunctionFlags;                                     // 0x00B8(0x0004)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_BC[0x1C];                                      // 0x00BC(0x001C)(Fixing Size After Last Property [ Dumper-7 ])
	FNativeFuncPtr                                Func;                                              // 0x00D8(0x0008)(NOT AUTO-GENERATED PROPERTY)

public:
	static class UClass* StaticClass()
	{
		return StaticClassImpl<"Function">();
	}
	static class UFunction* GetDefaultObj()
	{
		return GetDefaultObjImpl<UFunction>();
	}
};
static_assert(alignof(UFunction) == 0x000008, "Wrong alignment on UFunction");
static_assert(sizeof(UFunction) == 0x0000E0, "Wrong size on UFunction");
static_assert(offsetof(UFunction, NumParms) == 0x0000B0, "Member 'UFunction::NumParms' has a wrong offset!");
static_assert(offsetof(UFunction, ParmsSize) == 0x0000B2, "Member 'UFunction::ParmsSize' has a wrong offset!");
static_assert(offsetof(UFunction, ReturnValueOffset) == 0x0000B4, "Member 'UFunction::ReturnValueOffset' has a wrong offset!");
static_assert(offsetof(UFunction, FunctionFlags) == 0x0000B8, "Member 'UFunction::FunctionFlags' has a wrong offset!");
static_assert(offsetof(UFunction, Func) == 0x0000D8, "Member 'UFunction::Func' has a wrong offset!");


class UClass : public UStruct {
public:
	uint8                                         Pad_B0[0x28];                                      // 0x00B0(0x0028)(Fixing Size After Last Property [ Dumper-7 ])
	EClassCastFlags                               CastFlags;                                         // 0x00D8(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_E0[0x58];                                      // 0x00E0(0x0058)(Fixing Size After Last Property [ Dumper-7 ])
	class UObject*                                DefaultObject;                                     // 0x0138(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_140[0x140];                                    // 0x0140(0x0140)(Fixing Struct Size After Last Property [ Dumper-7 ])

public:
	class UFunction* GetFunction(const std::string& ClassName, const std::string& FuncName) const;

public:
	static class UClass* StaticClass()
	{
		return StaticClassImpl<"Class">();
	}
	static class UClass* GetDefaultObj()
	{
		return GetDefaultObjImpl<UClass>();
	}
};
static_assert(alignof(UClass) == 0x000008, "Wrong alignment on UClass");
static_assert(sizeof(UClass) == 0x000280, "Wrong size on UClass");
static_assert(offsetof(UClass, CastFlags) == 0x0000D8, "Member 'UClass::CastFlags' has a wrong offset!");
static_assert(offsetof(UClass, DefaultObject) == 0x000138, "Member 'UClass::DefaultObject' has a wrong offset!");


}
