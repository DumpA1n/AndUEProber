#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <functional>
#include <type_traits>

#include "UnrealContainers.h"

#include "Core/ElfScannerManager.h"
#include "Utils/KittyEx.h"

namespace SDK {

using namespace UC;

namespace Offsets
{
	constexpr int32 GObjects           = 0x1A36A768;
	constexpr int32 GetPlainANSIString = 0x00000000; // FName::GetPlainANSIString offset (also update the FName::GetPlainANSIString function implementation)
	constexpr int32 ProcessEventIdx    = 0x00000045;
}

namespace InSDKUtils
{
	uintptr_t GetImageBase();

	template<typename FuncType>
	inline FuncType GetVirtualFunction(const void* ObjectInstance, int32 Index)
	{
		void** VTable = *reinterpret_cast<void***>(const_cast<void*>(ObjectInstance));

		return reinterpret_cast<FuncType>(VTable[Index]);
	}

	template<typename FuncType, typename... ParamTypes>
	requires std::invocable<FuncType, ParamTypes...>
	inline auto CallGameFunction(FuncType Function, ParamTypes&&... Args)
	{
		return Function(std::forward<ParamTypes>(Args)...);
	}
}


template<int32 Len>
struct StringLiteral
{
	char Chars[Len];

	consteval StringLiteral(const char(&String)[Len])
	{
		std::copy_n(String, Len, Chars);
	}

	operator std::string() const
	{
		return static_cast<const char*>(Chars);
	}
};

// Forward declarations because in-line forward declarations make the compiler think 'StaticClassImpl()' is a class template
class UClass;
class UObject;
class UFunction;

class FName;

namespace BasicFilesImpleUtils
{
	// Helper functions for StaticClassImpl and StaticBPGeneratedClassImpl
	UClass* FindClassByName(const std::string& Name);
	UClass* FindClassByFullName(const std::string& Name);

	std::string GetObjectName(class UClass* Class);
	int32 GetObjectIndex(class UClass* Class);

	/* FName represented as a uint64. */
	uint64 GetObjFNameAsUInt64(class UClass* Class);

	UObject* GetObjectByIndex(int32 Index);

	UFunction* FindFunctionByFName(const FName* Name);
}

template<StringLiteral Name, bool bIsFullName = false>
class UClass* StaticClassImpl()
{
	static class UClass* Clss = nullptr;

	if (Clss == nullptr)
	{
		if constexpr (bIsFullName) {
			Clss = BasicFilesImpleUtils::FindClassByFullName(Name);
		}
		else /* default */ {
			Clss = BasicFilesImpleUtils::FindClassByName(Name);
		}
	}

	return Clss;
}

template<StringLiteral Name, bool bIsFullName = false, StringLiteral NonFullName = "">
class UClass* StaticBPGeneratedClassImpl()
{
	/* Could be external function, not really unique to this StaticClass functon */
	static auto SetClassIndex = [](UClass* Class, int32& Index, uint64& ClassName) -> UClass*
	{
		if (Class)
		{
			Index = BasicFilesImpleUtils::GetObjectIndex(Class);
			ClassName = BasicFilesImpleUtils::GetObjFNameAsUInt64(Class);
		}

		return Class;
	};

	static int32 ClassIdx = 0x0;
	static uint64 ClassName = 0x0;

	/* Use the full name to find an object */
	if constexpr (bIsFullName)
	{
		if (ClassIdx == 0x0) [[unlikely]]
			return SetClassIndex(BasicFilesImpleUtils::FindClassByFullName(Name), ClassIdx, ClassName);

		// UClass* ClassObj = static_cast<UClass*>(BasicFilesImpleUtils::GetObjectByIndex(ClassIdx));
		UClass* ClassObj = (UClass*)(BasicFilesImpleUtils::GetObjectByIndex(ClassIdx));

		/* Could use cast flags too to save some string comparisons */
		if (!ClassObj || BasicFilesImpleUtils::GetObjFNameAsUInt64(ClassObj) != ClassName)
			return SetClassIndex(BasicFilesImpleUtils::FindClassByFullName(Name), ClassIdx, ClassName);

		return ClassObj;
	}
	else /* Default, use just the name to find an object*/
	{
		if (ClassIdx == 0x0) [[unlikely]]
			return SetClassIndex(BasicFilesImpleUtils::FindClassByName(Name), ClassIdx, ClassName);

		// UClass* ClassObj = static_cast<UClass*>(BasicFilesImpleUtils::GetObjectByIndex(ClassIdx));
		UClass* ClassObj = (UClass*)(BasicFilesImpleUtils::GetObjectByIndex(ClassIdx));

		/* Could use cast flags too to save some string comparisons */
		if (!ClassObj || BasicFilesImpleUtils::GetObjFNameAsUInt64(ClassObj) != ClassName)
			return SetClassIndex(BasicFilesImpleUtils::FindClassByName(Name), ClassIdx, ClassName);

		return ClassObj;
	}
}

template<class ClassType>
ClassType* GetDefaultObjImpl()
{
	return reinterpret_cast<ClassType*>(ClassType::StaticClass()->DefaultObject);
}

template <typename To, typename From>
inline To* Cast(From* Src)
{
	if (!Src)
		return nullptr;

	if (Src->IsA(To::StaticClass()))
		return static_cast<To*>(Src);

	return nullptr;
}

struct FUObjectItem final
{
public:
	class UObject*                                Object;                                            // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_8[0x10];                                       // 0x0008(0x0010)(Fixing Struct Size After Last Property [ Dumper-7 ])
};

class TUObjectArray final
{
public:
	static constexpr auto DecryptPtr = [](void* ObjPtr) -> uint8*
	{
		return reinterpret_cast<uint8*>(ObjPtr);
	};

	int32                                         NumElementsPerChunk = 0x10000;                     // >0 = chunked, 0 = flat

	struct FUObjectItem**                         Objects;                                           // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_8[0x8];                                        // 0x0008(0x0008)(Fixing Size After Last Property [ Dumper-7 ])
	int32                                         MaxElements;                                       // 0x0010(0x0004)(NOT AUTO-GENERATED PROPERTY)
	int32                                         NumElements;                                       // 0x0014(0x0004)(NOT AUTO-GENERATED PROPERTY)
	int32                                         MaxChunks;                                         // 0x0018(0x0004)(NOT AUTO-GENERATED PROPERTY)
	int32                                         NumChunks;                                         // 0x001C(0x0004)(NOT AUTO-GENERATED PROPERTY)

public:
	inline int32 Num() const
	{
		return NumElements;
	}

	FUObjectItem** GetDecryptedObjPtr() const
	{
		return reinterpret_cast<FUObjectItem**>(DecryptPtr(Objects));
	}

	inline class UObject* GetByIndex(const int32 Index) const
	{
		if (Index < 0 || Index >= NumElements || !Objects)
			return nullptr;

		if (NumElementsPerChunk <= 0) {
			return KT::Read<UObject*>(reinterpret_cast<uintptr_t>(Objects) + Index * sizeof(FUObjectItem) + offsetof(FUObjectItem, Object));
		}

		const int32_t ChunkIndex = Index / NumElementsPerChunk;
		const int32_t WithinChunkIndex = Index % NumElementsPerChunk;

		// if (ChunkIndex >= NumChunks) return nullptr;

		uint64_t chunk = KT::Read<uint64_t>(Objects + ChunkIndex);
		if (!chunk)
			return nullptr;

		return KT::Read<UObject*>(chunk + (WithinChunkIndex * sizeof(FUObjectItem)) + offsetof(FUObjectItem, Object));
	}

	void ForEachObject(const std::function<bool(UObject*)> &callback) const
	{
		if (!callback) return;

		for (int32_t i = 0; i < NumElements; i++)
		{
			UObject* object = GetByIndex(i);
			if (!object) continue;

			if (callback(object)) return;
		}
	}
};

struct TUObjectArrayWrapper
{
private:
	friend class UObject;

private:
	void* GObjectsAddress = nullptr;

private:
	TUObjectArrayWrapper() = default;

public:
	TUObjectArrayWrapper(TUObjectArrayWrapper&&) = delete;
	TUObjectArrayWrapper(const TUObjectArrayWrapper&) = delete;

	TUObjectArrayWrapper& operator=(TUObjectArrayWrapper&&) = delete;
	TUObjectArrayWrapper& operator=(const TUObjectArrayWrapper&) = delete;

private:
	inline void InitGObjects()
	{
        auto _t = new TUObjectArray();
		_t->Objects = KT::Read<FUObjectItem**>(Elf.UE4().base() + Offsets::GObjects);
        _t->MaxElements = 327680;
        _t->NumElements = 327680;
        _t->MaxChunks = 5;
        _t->NumChunks = 5;
        GObjectsAddress = _t;

		// GObjectsAddress = reinterpret_cast<void*>(Elf.UE4().base() + Offsets::GObjects);
	}

public:
	inline void InitManually(void* GObjectsAddressParameter)
	{
		GObjectsAddress = GObjectsAddressParameter;
	}

	inline class TUObjectArray* operator->()
	{
		if (!GObjectsAddress) [[unlikely]]
			InitGObjects();

		return reinterpret_cast<class TUObjectArray*>(GObjectsAddress);
	}

	inline TUObjectArray& operator*() const
	{
		return *reinterpret_cast<class TUObjectArray*>(GObjectsAddress);
	}

	inline operator const void* ()
	{
		if (!GObjectsAddress) [[unlikely]]
			InitGObjects();

		return GObjectsAddress;
	}

	inline class TUObjectArray* GetTypedPtr()
	{
		if (!GObjectsAddress) [[unlikely]]
			InitGObjects();

		return reinterpret_cast<class TUObjectArray*>(GObjectsAddress);
	}
};

class FName final
{
public:
	static inline void*                           AppendString = nullptr;                            // 0x0000(0x0004)(NOT AUTO-GENERATED PROPERTY)

	// Runtime name resolver — set by DumperBridge to use profile's GetNameByID
	static inline std::function<std::string(int32_t)> s_NameResolver;

#define bWITH_CASE_PRESERVING_NAME false
#if !bWITH_CASE_PRESERVING_NAME
	union {
#endif
	int32                                         ComparisonIndex;                                   // 0x0000(0x0004)(NOT AUTO-GENERATED PROPERTY)
	int32                                         DisplayIndex;                                      // 0x0004(0x0004)(NOT AUTO-GENERATED PROPERTY)
#if !bWITH_CASE_PRESERVING_NAME
	};
#endif
	uint32                                        Number;                                            // 0x0008(0x0004)(NOT AUTO-GENERATED PROPERTY)

public:
	int32 GetDisplayIndex() const
	{
		return DisplayIndex;
	}

	static std::string GetPlainANSIString(const FName* Name)
	{
		// If a runtime resolver is set (from profile's GetNameByID), use it
		if (s_NameResolver) {
			return s_NameResolver(Name->ComparisonIndex);
		}

		// Fallback: hardcoded DeltaForce FNamePool access
		char buf[1024] = {0};
		uint32_t v10 = Name->ComparisonIndex;
        uint16_t *v16 = (uint16_t *)(*(uint64_t *)(Elf.UE4().base() + 0x1A343A00 + ((v10 >> 15) & 0x1FFF8) + 56) + 2 * (v10 & 0x3FFFFLL));
        uint32_t len = *v16 >> 6;
		memcpy(buf, v16 + 1, len > 1024 ? 1024 : len);
		((void(*)(char*, uint32_t))(Elf.UE4().base() + 0xD6C40FC))(buf, len);

		return std::string(buf);
	}
	
	std::string GetRawString() const
	{
		return GetPlainANSIString(this);
	}
	
	std::string ToString() const
	{
		std::string OutputString = GetRawString();
	
		size_t pos = OutputString.rfind('/');
	
		if (pos == std::string::npos)
			return OutputString;
	
		return OutputString.substr(pos + 1);
	}

	const char* ToCString() const
	{
		return ToString().c_str();
	}
	
	bool operator==(const FName& Other) const
	{
		return ComparisonIndex == Other.ComparisonIndex && Number == Other.Number;
	}
	bool operator!=(const FName& Other) const
	{
		return ComparisonIndex != Other.ComparisonIndex || Number != Other.Number;
	}
};

template<typename ClassType>
class TSubclassOf
{
	class UClass* ClassPtr;

public:
	TSubclassOf() = default;

	inline TSubclassOf(UClass* Class)
		: ClassPtr(Class)
	{
	}

	inline UClass* Get()
	{
		return ClassPtr;
	}

	inline operator UClass*() const
	{
		return ClassPtr;
	}

	template<typename Target, typename = std::enable_if<std::is_base_of_v<Target, ClassType>, bool>::type>
	inline operator TSubclassOf<Target>() const
	{
		return ClassPtr;
	}

	inline UClass* operator->()
	{
		return ClassPtr;
	}

	inline TSubclassOf& operator=(UClass* Class)
	{
		ClassPtr = Class;

		return *this;
	}

	inline bool operator==(const TSubclassOf& Other) const
	{
		return ClassPtr == Other.ClassPtr;
	}

	inline bool operator!=(const TSubclassOf& Other) const
	{
		return ClassPtr != Other.ClassPtr;
	}

	inline bool operator==(UClass* Other) const
	{
		return ClassPtr == Other;
	}

	inline bool operator!=(UClass* Other) const
	{
		return ClassPtr != Other;
	}
};

namespace FTextImpl
{
// Predefined struct FTextData
// 0x0038 (0x0038 - 0x0000)
class FTextData final
{
public:
	uint8                                         Pad_0[0x28];                                       // 0x0000(0x0028)(Fixing Size After Last Property [ Dumper-7 ])
	class FString                                 TextSource;                                        // 0x0028(0x0010)(NOT AUTO-GENERATED PROPERTY)
};
}

// Predefined struct FText
// 0x0018 (0x0018 - 0x0000)
class FText final
{
public:
	class FTextImpl::FTextData*                   TextData;                                          // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_8[0x10];                                       // 0x0008(0x0010)(Fixing Struct Size After Last Property [ Dumper-7 ])

public:
	const class FString& GetStringRef() const
	{
		if (!TextData)
			return FString();
		return TextData->TextSource;
	}
	std::string ToString() const
	{
		if (!TextData)
			return "";
		return TextData->TextSource.ToString();
	}
	bool IsValid() const
	{
		return TextData != nullptr;
	}
};

class FWeakObjectPtr
{
public:
	int32_t                                       ObjectIndex;                                       // 0x0000(0x0004)(NOT AUTO-GENERATED PROPERTY)
	int32_t                                       ObjectSerialNumber;                                // 0x0004(0x0004)(NOT AUTO-GENERATED PROPERTY)

public:
	class UObject* Get() const;
	class UObject* operator->() const;
	bool operator==(const FWeakObjectPtr& Other) const;
	bool operator!=(const FWeakObjectPtr& Other) const;
	bool operator==(const class UObject* Other) const;
	bool operator!=(const class UObject* Other) const;
};

template<typename UEType>
class TWeakObjectPtr : public FWeakObjectPtr
{
public:
	UEType* Get() const {
		return static_cast<UEType*>(FWeakObjectPtr::Get());
	}

    bool IsValid() const {
        return Get() != nullptr;
    }

	UEType* operator->() const {
		return static_cast<UEType*>(FWeakObjectPtr::Get());
	}
};

// Predefined struct FUniqueObjectGuid
// 0x0010 (0x0010 - 0x0000)
class FUniqueObjectGuid final
{
public:
	uint32                                        A;                                                 // 0x0000(0x0004)(NOT AUTO-GENERATED PROPERTY)
	uint32                                        B;                                                 // 0x0004(0x0004)(NOT AUTO-GENERATED PROPERTY)
	uint32                                        C;                                                 // 0x0008(0x0004)(NOT AUTO-GENERATED PROPERTY)
	uint32                                        D;                                                 // 0x000C(0x0004)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FUniqueObjectGuid) == 0x000004, "Wrong alignment on FUniqueObjectGuid");
static_assert(sizeof(FUniqueObjectGuid) == 0x000010, "Wrong size on FUniqueObjectGuid");
static_assert(offsetof(FUniqueObjectGuid, A) == 0x000000, "Member 'FUniqueObjectGuid::A' has a wrong offset!");
static_assert(offsetof(FUniqueObjectGuid, B) == 0x000004, "Member 'FUniqueObjectGuid::B' has a wrong offset!");
static_assert(offsetof(FUniqueObjectGuid, C) == 0x000008, "Member 'FUniqueObjectGuid::C' has a wrong offset!");
static_assert(offsetof(FUniqueObjectGuid, D) == 0x00000C, "Member 'FUniqueObjectGuid::D' has a wrong offset!");

// Predefined struct TPersistentObjectPtr
// 0x0000 (0x0000 - 0x0000)
template<typename TObjectID>
class TPersistentObjectPtr
{
public:
	FWeakObjectPtr                                WeakPtr;                                           // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	int32                                         TagAtLastTest;                                     // 0x0008(0x0004)(NOT AUTO-GENERATED PROPERTY)
	TObjectID                                     ObjectID;                                          // 0x000C(0x0000)(NOT AUTO-GENERATED PROPERTY)

public:
	class UObject* Get() const
	{
		return WeakPtr.Get();
	}
	class UObject* operator->() const
	{
		return WeakPtr.Get();
	}
};

template<typename UEType>
class TLazyObjectPtr : public TPersistentObjectPtr<FUniqueObjectGuid>
{
public:
	UEType* Get() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
	UEType* operator->() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
};

namespace FakeSoftObjectPtr
{

// ScriptStruct CoreUObject.SoftObjectPath
// 0x0020 (0x0020 - 0x0000)
struct FSoftObjectPath
{
public:
	class FName                                   AssetPathName;                                     // 0x0000(0x000C)(ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
	uint8                                         Pad_C[0x4];                                        // 0x000C(0x0004)(Fixing Size After Last Property [ Dumper-7 ])
	class FString                                 SubPathString;                                     // 0x0010(0x0010)(ZeroConstructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
};
static_assert(alignof(FSoftObjectPath) == 0x000008, "Wrong alignment on FSoftObjectPath");
static_assert(sizeof(FSoftObjectPath) == 0x000020, "Wrong size on FSoftObjectPath");
static_assert(offsetof(FSoftObjectPath, AssetPathName) == 0x000000, "Member 'FSoftObjectPath::AssetPathName' has a wrong offset!");
static_assert(offsetof(FSoftObjectPath, SubPathString) == 0x000010, "Member 'FSoftObjectPath::SubPathString' has a wrong offset!");

}

class FSoftObjectPtr : public TPersistentObjectPtr<FakeSoftObjectPtr::FSoftObjectPath>
{
};

template<typename UEType>
class TSoftObjectPtr : public FSoftObjectPtr
{
public:
	UEType* Get() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
	UEType* operator->() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
};

template<typename UEType>
class TSoftClassPtr : public FSoftObjectPtr
{
public:
	UEType* Get() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
	UEType* operator->() const
	{
		return static_cast<UEType*>(TPersistentObjectPtr::Get());
	}
};

// Predefined struct FScriptInterface
// 0x0010 (0x0010 - 0x0000)
class FScriptInterface
{
public:
	UObject*                                      ObjectPointer;                                     // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	void*                                         InterfacePointer;                                  // 0x0008(0x0008)(NOT AUTO-GENERATED PROPERTY)

public:
	class UObject* GetObjectRef() const
	{
		return ObjectPointer;
	}
	
	void* GetInterfaceRef() const
	{
		return InterfacePointer;
	}
	
};
static_assert(alignof(FScriptInterface) == 0x000008, "Wrong alignment on FScriptInterface");
static_assert(sizeof(FScriptInterface) == 0x000010, "Wrong size on FScriptInterface");
static_assert(offsetof(FScriptInterface, ObjectPointer) == 0x000000, "Member 'FScriptInterface::ObjectPointer' has a wrong offset!");
static_assert(offsetof(FScriptInterface, InterfacePointer) == 0x000008, "Member 'FScriptInterface::InterfacePointer' has a wrong offset!");

// Predefined struct TScriptInterface
// 0x0000 (0x0010 - 0x0010)
template<class InterfaceType>
class TScriptInterface final : public FScriptInterface
{
};

// Predefined struct FFieldPath
// 0x0020 (0x0020 - 0x0000)
class FFieldPath
{
public:
	class FField*                                 ResolvedField;                                     // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	TWeakObjectPtr<class UStruct>                 ResolvedOwner;                                     // 0x0008(0x0008)(NOT AUTO-GENERATED PROPERTY)
	TArray<FName>                                 Path;                                              // 0x0010(0x0010)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FFieldPath) == 0x000008, "Wrong alignment on FFieldPath");
static_assert(sizeof(FFieldPath) == 0x000020, "Wrong size on FFieldPath");
static_assert(offsetof(FFieldPath, ResolvedField) == 0x000000, "Member 'FFieldPath::ResolvedField' has a wrong offset!");
static_assert(offsetof(FFieldPath, ResolvedOwner) == 0x000008, "Member 'FFieldPath::ResolvedOwner' has a wrong offset!");
static_assert(offsetof(FFieldPath, Path) == 0x000010, "Member 'FFieldPath::Path' has a wrong offset!");

// Predefined struct TFieldPath
// 0x0000 (0x0020 - 0x0020)
template<class PropertyType>
class TFieldPath final : public FFieldPath
{
};


template<typename OptionalType, bool bIsIntrusiveUnsetCheck = false>
class TOptional
{
private:
	template<int32 TypeSize>
	struct OptionalWithBool
	{
		static_assert(TypeSize > 0x0, "TOptional can not store an empty type!");

		uint8 Value[TypeSize];
		bool bIsSet;
	};

private:
	using ValueType = std::conditional_t<bIsIntrusiveUnsetCheck, uint8[sizeof(OptionalType)], OptionalWithBool<sizeof(OptionalType)>>;

private:
	alignas(OptionalType) ValueType StoredValue;

private:
	inline uint8* GetValueBytes()
	{
		if constexpr (!bIsIntrusiveUnsetCheck)
			return StoredValue.Value;

		return StoredValue;
	}

	inline const uint8* GetValueBytes() const
	{
		if constexpr (!bIsIntrusiveUnsetCheck)
			return StoredValue.Value;

		return StoredValue;
	}
public:

	inline OptionalType& GetValueRef()
	{
		return *reinterpret_cast<OptionalType*>(GetValueBytes());
	}

	inline const OptionalType& GetValueRef() const
	{
		return *reinterpret_cast<const OptionalType*>(GetValueBytes());
	}

	inline bool IsSet() const
	{
		if constexpr (!bIsIntrusiveUnsetCheck)
			return StoredValue.bIsSet;

		constexpr char ZeroBytes[sizeof(OptionalType)];

		return memcmp(GetValueBytes(), &ZeroBytes, sizeof(OptionalType)) == 0;
	}

	inline explicit operator bool() const
	{
		return IsSet();
	}
};


// Predefined struct FScriptDelegate
// 0x0014 (0x0014 - 0x0000)
struct FScriptDelegate
{
public:
	FWeakObjectPtr                                Object;                                            // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	FName                                         FunctionName;                                      // 0x0008(0x000C)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FScriptDelegate) == 0x000004, "Wrong alignment on FScriptDelegate");
static_assert(sizeof(FScriptDelegate) == 0x000010, "Wrong size on FScriptDelegate");
static_assert(offsetof(FScriptDelegate, Object) == 0x000000, "Member 'FScriptDelegate::Object' has a wrong offset!");
static_assert(offsetof(FScriptDelegate, FunctionName) == 0x000008, "Member 'FScriptDelegate::FunctionName' has a wrong offset!");

// Predefined struct TDelegate
// 0x0028 (0x0028 - 0x0000)
template<typename FunctionSignature>
class TDelegate
{
public:
	// struct InvalidUseOfTDelegate                  TemplateParamIsNotAFunctionSignature;              // 0x0000(0x0000)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_0[0x28];                                       // 0x0000(0x0028)(Fixing Struct Size After Last Property [ Dumper-7 ])
};

// Predefined struct TDelegate<Ret(Args...)>
// 0x0028 (0x0028 - 0x0000)
template<typename Ret, typename... Args>
class TDelegate<Ret(Args...)>
{
public:
	FScriptDelegate                               BoundFunction;                                     // 0x0000(0x0014)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_14[0x14];                                      // 0x0014(0x0014)(Fixing Struct Size After Last Property [ Dumper-7 ])
};

// Predefined struct TMulticastInlineDelegate
// 0x0010 (0x0010 - 0x0000)
template<typename FunctionSignature>
class TMulticastInlineDelegate
{
public:
	// struct InvalidUseOfTMulticastInlineDelegate   TemplateParamIsNotAFunctionSignature;              // 0x0000(0x0014)(NOT AUTO-GENERATED PROPERTY)
};

// Predefined struct TMulticastInlineDelegate<Ret(Args...)>
// 0x0000 (0x0000 - 0x0000)
template<typename Ret, typename... Args>
class TMulticastInlineDelegate<Ret(Args...)>
{
public:
	TArray<FScriptDelegate>                       InvocationList;                                    // 0x0000(0x0010)(NOT AUTO-GENERATED PROPERTY)
};

#define UE_ENUM_OPERATORS(EEnumClass)																																	\
																																										\
inline constexpr EEnumClass operator|(EEnumClass Left, EEnumClass Right)																								\
{																																										\
	return (EEnumClass)((std::underlying_type<EEnumClass>::type)(Left) | (std::underlying_type<EEnumClass>::type)(Right));												\
}																																										\
																																										\
inline constexpr EEnumClass& operator|=(EEnumClass& Left, EEnumClass Right)																								\
{																																										\
	return (EEnumClass&)((std::underlying_type<EEnumClass>::type&)(Left) |= (std::underlying_type<EEnumClass>::type)(Right));											\
}																																										\
																																										\
inline bool operator&(EEnumClass Left, EEnumClass Right)																												\
{																																										\
	return (((std::underlying_type<EEnumClass>::type)(Left) & (std::underlying_type<EEnumClass>::type)(Right)) == (std::underlying_type<EEnumClass>::type)(Right));		\
}

enum class EObjectFlags : int32
{
	NoFlags							= 0x00000000,

	Public							= 0x00000001,
	Standalone						= 0x00000002,
	MarkAsNative					= 0x00000004,
	Transactional					= 0x00000008,
	ClassDefaultObject				= 0x00000010,
	ArchetypeObject					= 0x00000020,
	Transient						= 0x00000040,

	MarkAsRootSet					= 0x00000080,
	TagGarbageTemp					= 0x00000100,

	NeedInitialization				= 0x00000200,
	NeedLoad						= 0x00000400,
	KeepForCooker					= 0x00000800,
	NeedPostLoad					= 0x00001000,
	NeedPostLoadSubobjects			= 0x00002000,
	NewerVersionExists				= 0x00004000,
	BeginDestroyed					= 0x00008000,
	FinishDestroyed					= 0x00010000,

	BeingRegenerated				= 0x00020000,
	DefaultSubObject				= 0x00040000,
	WasLoaded						= 0x00080000,
	TextExportTransient				= 0x00100000,
	LoadCompleted					= 0x00200000,
	InheritableComponentTemplate	= 0x00400000,
	DuplicateTransient				= 0x00800000,
	StrongRefOnFrame				= 0x01000000,
	NonPIEDuplicateTransient		= 0x02000000,
	Dynamic							= 0x04000000,
	WillBeLoaded					= 0x08000000,
	HasExternalPackage				= 0x10000000,

	MirroredGarbage					= 0x40000000,
	// AllocatedInSharedPage			= 0x80000000,
};

enum class EFunctionFlags : uint32
{
	FUNC_None				        = 0x00000000,

	FUNC_Final				        = 0x00000001,	// Function is final (prebindable, non-overridable function).
	FUNC_RequiredAPI			    = 0x00000002,	// Indicates this function is DLL exported/imported.
	FUNC_BlueprintAuthorityOnly     = 0x00000004,   // Function will only run if the object has network authority
	FUNC_BlueprintCosmetic	        = 0x00000008,   // Function is cosmetic in nature and should not be invoked on dedicated servers
	// FUNC_				        = 0x00000010,   // unused.
	// FUNC_				        = 0x00000020,   // unused.
	FUNC_Net				        = 0x00000040,   // Function is network-replicated.
	FUNC_NetReliable		        = 0x00000080,   // Function should be sent reliably on the network.
	FUNC_NetRequest			        = 0x00000100,	// Function is sent to a net service
	FUNC_Exec				        = 0x00000200,	// Executable from command line.
	FUNC_Native				        = 0x00000400,	// Native function.
	FUNC_Event				        = 0x00000800,   // Event function.
	FUNC_NetResponse		        = 0x00001000,   // Function response from a net service
	FUNC_Static				        = 0x00002000,   // Static function.
	FUNC_NetMulticast		        = 0x00004000,	// Function is networked multicast Server -> All Clients
	FUNC_UbergraphFunction	        = 0x00008000,   // Function is used as the merge 'ubergraph' for a blueprint, only assigned when using the persistent 'ubergraph' frame
	FUNC_MulticastDelegate	        = 0x00010000,	// Function is a multi-cast delegate signature (also requires FUNC_Delegate to be set!)
	FUNC_Public				        = 0x00020000,	// Function is accessible in all classes (if overridden, parameters must remain unchanged).
	FUNC_Private			        = 0x00040000,	// Function is accessible only in the class it is defined in (cannot be overridden, but function name may be reused in subclasses.  IOW: if overridden, parameters don't need to match, and Super.Func() cannot be accessed since it's private.)
	FUNC_Protected			        = 0x00080000,	// Function is accessible only in the class it is defined in and subclasses (if overridden, parameters much remain unchanged).
	FUNC_Delegate			        = 0x00100000,	// Function is delegate signature (either single-cast or multi-cast, depending on whether FUNC_MulticastDelegate is set.)
	FUNC_NetServer			        = 0x00200000,	// Function is executed on servers (set by replication code if passes check)
	FUNC_HasOutParms		        = 0x00400000,	// function has out (pass by reference) parameters
	FUNC_HasDefaults		        = 0x00800000,	// function has structs that contain defaults
	FUNC_NetClient			        = 0x01000000,	// function is executed on clients
	FUNC_DLLImport			        = 0x02000000,	// function is imported from a DLL
	FUNC_BlueprintCallable	        = 0x04000000,	// function can be called from blueprint code
	FUNC_BlueprintEvent		        = 0x08000000,	// function can be overridden/implemented from a blueprint
	FUNC_BlueprintPure		        = 0x10000000,	// function can be called from blueprint code, and is also pure (produces no side effects). If you set this, you should set FUNC_BlueprintCallable as well.
	FUNC_EditorOnly			        = 0x20000000,	// function can only be called from an editor script.
	FUNC_Const				        = 0x40000000,	// function can be called from blueprint code, and only reads state (never writes state)
	FUNC_NetValidate		        = 0x80000000,	// function must supply a _Validate implementation

	FUNC_AllFlags		            = 0xFFFFFFFF,
};

enum class EClassFlags : uint32
{
	CLASS_None						= 0x00000000u,
	Abstract						= 0x00000001u,
	DefaultConfig					= 0x00000002u,
	Config							= 0x00000004u,
	Transient						= 0x00000008u,
	Parsed							= 0x00000010u,
	MatchedSerializers				= 0x00000020u,
	ProjectUserConfig				= 0x00000040u,
	Native							= 0x00000080u,
	NoExport						= 0x00000100u,
	NotPlaceable					= 0x00000200u,
	PerObjectConfig					= 0x00000400u,
	ReplicationDataIsSetUp			= 0x00000800u,
	EditInlineNew					= 0x00001000u,
	CollapseCategories				= 0x00002000u,
	Interface						= 0x00004000u,
	CustomConstructor				= 0x00008000u,
	Const							= 0x00010000u,
	LayoutChanging					= 0x00020000u,
	CompiledFromBlueprint			= 0x00040000u,
	MinimalAPI						= 0x00080000u,
	RequiredAPI						= 0x00100000u,
	DefaultToInstanced				= 0x00200000u,
	TokenStreamAssembled			= 0x00400000u,
	HasInstancedReference			= 0x00800000u,
	Hidden							= 0x01000000u,
	Deprecated						= 0x02000000u,
	HideDropDown					= 0x04000000u,
	GlobalUserConfig				= 0x08000000u,
	Intrinsic						= 0x10000000u,
	Constructed						= 0x20000000u,
	ConfigDoNotCheckDefaults		= 0x40000000u,
	NewerVersionExists				= 0x80000000u,
};

enum class EClassCastFlags : uint64
{
	None = 0x0000000000000000,

	Field								= 0x0000000000000001,
	Int8Property						= 0x0000000000000002,
	Enum								= 0x0000000000000004,
	Struct								= 0x0000000000000008,
	ScriptStruct						= 0x0000000000000010,
	Class								= 0x0000000000000020,
	ByteProperty						= 0x0000000000000040,
	IntProperty							= 0x0000000000000080,
	FloatProperty						= 0x0000000000000100,
	UInt64Property						= 0x0000000000000200,
	ClassProperty						= 0x0000000000000400,
	UInt32Property						= 0x0000000000000800,
	InterfaceProperty					= 0x0000000000001000,
	NameProperty						= 0x0000000000002000,
	StrProperty							= 0x0000000000004000,
	Property							= 0x0000000000008000,
	ObjectProperty						= 0x0000000000010000,
	BoolProperty						= 0x0000000000020000,
	UInt16Property						= 0x0000000000040000,
	Function							= 0x0000000000080000,
	StructProperty						= 0x0000000000100000,
	ArrayProperty						= 0x0000000000200000,
	Int64Property						= 0x0000000000400000,
	DelegateProperty					= 0x0000000000800000,
	NumericProperty						= 0x0000000001000000,
	MulticastDelegateProperty			= 0x0000000002000000,
	ObjectPropertyBase					= 0x0000000004000000,
	WeakObjectProperty					= 0x0000000008000000,
	LazyObjectProperty					= 0x0000000010000000,
	SoftObjectProperty					= 0x0000000020000000,
	TextProperty						= 0x0000000040000000,
	Int16Property						= 0x0000000080000000,
	DoubleProperty						= 0x0000000100000000,
	SoftClassProperty					= 0x0000000200000000,
	Package								= 0x0000000400000000,
	Level								= 0x0000000800000000,
	Actor								= 0x0000001000000000,
	PlayerController					= 0x0000002000000000,
	Pawn								= 0x0000004000000000,
	SceneComponent						= 0x0000008000000000,
	PrimitiveComponent					= 0x0000010000000000,
	SkinnedMeshComponent				= 0x0000020000000000,
	SkeletalMeshComponent				= 0x0000040000000000,
	Blueprint							= 0x0000080000000000,
	DelegateFunction					= 0x0000100000000000,
	StaticMeshComponent					= 0x0000200000000000,
	MapProperty							= 0x0000400000000000,
	SetProperty							= 0x0000800000000000,
	EnumProperty						= 0x0001000000000000,
	USparseDelegateFunction				= 0x0002000000000000,
	MulticastInlineDelegateProperty	    = 0x0004000000000000,
	MulticastSparseDelegateProperty	    = 0x0008000000000000,
	FieldPathProperty					= 0x0010000000000000,
	LargeWorldCoordinatesRealProperty	= 0x0080000000000000,
	OptionalProperty					= 0x0100000000000000,
	VValueProperty						= 0x0200000000000000,
	UVerseVMClass						= 0x0400000000000000,
	VRestValueProperty					= 0x0800000000000000,
};

enum class EPropertyFlags : uint64
{
	None								= 0x0000000000000000,

	Edit								= 0x0000000000000001,
	ConstParm							= 0x0000000000000002,
	BlueprintVisible					= 0x0000000000000004,
	ExportObject						= 0x0000000000000008,
	BlueprintReadOnly					= 0x0000000000000010,
	Net									= 0x0000000000000020,
	EditFixedSize						= 0x0000000000000040,
	Parm								= 0x0000000000000080,
	OutParm								= 0x0000000000000100,
	ZeroConstructor						= 0x0000000000000200,
	ReturnParm							= 0x0000000000000400,
	DisableEditOnTemplate 				= 0x0000000000000800,

	Transient							= 0x0000000000002000,
	Config								= 0x0000000000004000,

	DisableEditOnInstance				= 0x0000000000010000,
	EditConst							= 0x0000000000020000,
	GlobalConfig						= 0x0000000000040000,
	InstancedReference					= 0x0000000000080000,	

	DuplicateTransient					= 0x0000000000200000,	
	SubobjectReference					= 0x0000000000400000,	

	SaveGame							= 0x0000000001000000,
	NoClear								= 0x0000000002000000,

	ReferenceParm						= 0x0000000008000000,
	BlueprintAssignable					= 0x0000000010000000,
	Deprecated							= 0x0000000020000000,
	IsPlainOldData						= 0x0000000040000000,
	RepSkip								= 0x0000000080000000,
	RepNotify							= 0x0000000100000000, 
	Interp								= 0x0000000200000000,
	NonTransactional					= 0x0000000400000000,
	EditorOnly							= 0x0000000800000000,
	NoDestructor						= 0x0000001000000000,

	AutoWeak							= 0x0000004000000000,
	ContainsInstancedReference			= 0x0000008000000000,	
	AssetRegistrySearchable				= 0x0000010000000000,
	SimpleDisplay						= 0x0000020000000000,
	AdvancedDisplay						= 0x0000040000000000,
	Protected							= 0x0000080000000000,
	BlueprintCallable					= 0x0000100000000000,
	BlueprintAuthorityOnly				= 0x0000200000000000,
	TextExportTransient					= 0x0000400000000000,
	NonPIEDuplicateTransient			= 0x0000800000000000,
	ExposeOnSpawn						= 0x0001000000000000,
	PersistentInstance					= 0x0002000000000000,
	UObjectWrapper						= 0x0004000000000000, 
	HasGetValueTypeHash					= 0x0008000000000000, 
	NativeAccessSpecifierPublic			= 0x0010000000000000,	
	NativeAccessSpecifierProtected		= 0x0020000000000000,
	NativeAccessSpecifierPrivate		= 0x0040000000000000,	
	SkipSerialization					= 0x0080000000000000, 
};

class FFieldClass
{
public:
	FName                                         Name;                                              // 0x0000(0x000C)(NOT AUTO-GENERATED PROPERTY)
	uint64                                        Id;                                                // 0x0010(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint64                                        CastFlags;                                         // 0x0018(0x0008)(NOT AUTO-GENERATED PROPERTY)
	EClassFlags                                   ClassFlags;                                        // 0x0020(0x0004)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_24[0x4];                                       // 0x0024(0x0004)(Fixing Size After Last Property [ Dumper-7 ])
	class FFieldClass*                            SuperClass;                                        // 0x0028(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FFieldClass) == 0x000008, "Wrong alignment on FFieldClass");
static_assert(sizeof(FFieldClass) == 0x000028, "Wrong size on FFieldClass");
static_assert(offsetof(FFieldClass, Name) == 0x000000, "Member 'FFieldClass::Name' has a wrong offset!");
static_assert(offsetof(FFieldClass, Id) == 0x000008, "Member 'FFieldClass::Id' has a wrong offset!");
static_assert(offsetof(FFieldClass, CastFlags) == 0x000010, "Member 'FFieldClass::CastFlags' has a wrong offset!");
static_assert(offsetof(FFieldClass, ClassFlags) == 0x000018, "Member 'FFieldClass::ClassFlags' has a wrong offset!");
static_assert(offsetof(FFieldClass, SuperClass) == 0x000020, "Member 'FFieldClass::SuperClass' has a wrong offset!");

class FFieldVariant
{
public:
	using ContainerType = union { class FField* Field; class UObject* Object; };

	ContainerType                                 Container;                                         // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	bool                                          bIsUObject;                                        // 0x0008(0x0001)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FFieldVariant) == 0x000008, "Wrong alignment on FFieldVariant");
static_assert(sizeof(FFieldVariant) == 0x000010, "Wrong size on FFieldVariant");
static_assert(offsetof(FFieldVariant, Container) == 0x000000, "Member 'FFieldVariant::Container' has a wrong offset!");
static_assert(offsetof(FFieldVariant, bIsUObject) == 0x000008, "Member 'FFieldVariant::bIsUObject' has a wrong offset!");

class FField
{
public:
	~FField() {} // non-trivial destructor: enables Itanium ABI tail-padding reuse in derived classes

	void**                                        VTable;                                            // 0x0000(0x0008)(NOT AUTO-GENERATED PROPERTY)
	FFieldVariant                                 Owner;                                             // 0x0008(0x0010)(NOT AUTO-GENERATED PROPERTY)
	class FField*                                 Next;                                              // 0x0018(0x0008)(NOT AUTO-GENERATED PROPERTY)
	class FFieldClass*                            ClassPrivate;                                      // 0x0020(0x0008)(NOT AUTO-GENERATED PROPERTY)
	FName                                         NamePrivate;                                       // 0x0028(0x0008)(NOT AUTO-GENERATED PROPERTY)
	EObjectFlags                                  FlagsPrivate;                                      // 0x0030(0x0004)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FField) == 0x000008, "Wrong alignment on FField");
static_assert(sizeof(FField) == 0x000038, "Wrong size on FField");
static_assert(offsetof(FField, VTable) == 0x000000, "Member 'FField::VTable' has a wrong offset!");
static_assert(offsetof(FField, Owner) == 0x000008, "Member 'FField::Owner' has a wrong offset!");
static_assert(offsetof(FField, Next) == 0x000018, "Member 'FField::Next' has a wrong offset!");
static_assert(offsetof(FField, ClassPrivate) == 0x000020, "Member 'FField::ClassPrivate' has a wrong offset!");
static_assert(offsetof(FField, NamePrivate) == 0x000028, "Member 'FField::NamePrivate' has a wrong offset!");
static_assert(offsetof(FField, FlagsPrivate) == 0x000030, "Member 'FField::FlagsPrivate' has a wrong offset!");

// Predefined struct FProperty
// 0x0050 (0x0088 - 0x0038)
class FProperty : public FField
{
public:
	uint8                                         Pad_34[0x4];                                       // 0x0034(0x0004)(Fixing Size After Last Property [ Dumper-7 ])
	int32                                         ArrayDim;                                          // 0x0038(0x0004)(NOT AUTO-GENERATED PROPERTY)
	int32                                         ElementSize;                                       // 0x003C(0x0004)(NOT AUTO-GENERATED PROPERTY)
	EPropertyFlags                                PropertyFlags;                                     // 0x0040(0x0008)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_48[0x4];                                       // 0x0048(0x0004)(Fixing Size After Last Property [ Dumper-7 ])
	int32                                         Offset_Internal;                                   // 0x004C(0x0004)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         Pad_50[0x38];                                      // 0x0050(0x0038)(Fixing Struct Size After Last Property [ Dumper-7 ])
};
static_assert(alignof(FProperty) == 0x000008, "Wrong alignment on FProperty");
static_assert(sizeof(FProperty) == 0x000088, "Wrong size on FProperty");
static_assert(offsetof(FProperty, ArrayDim) == 0x000038, "Member 'FProperty::ArrayDim' has a wrong offset!");
static_assert(offsetof(FProperty, ElementSize) == 0x00003C, "Member 'FProperty::ElementSize' has a wrong offset!");
static_assert(offsetof(FProperty, PropertyFlags) == 0x000040, "Member 'FProperty::PropertyFlags' has a wrong offset!");
static_assert(offsetof(FProperty, Offset_Internal) == 0x00004C, "Member 'FProperty::Offset_Internal' has a wrong offset!");


// Predefined struct FByteProperty
// 0x0008 (0x0090 - 0x0088)
class FByteProperty final : public FProperty
{
public:
	class UEnum*                                  Enum;                                              // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FByteProperty) == 0x000008, "Wrong alignment on FByteProperty");
static_assert(sizeof(FByteProperty) == 0x000090, "Wrong size on FByteProperty");
static_assert(offsetof(FByteProperty, Enum) == 0x000088, "Member 'FByteProperty::Enum' has a wrong offset!");

// Predefined struct FBoolProperty
// 0x0008 (0x0090 - 0x0088)
class FBoolProperty final : public FProperty
{
public:
	uint8                                         FieldSize;                                         // 0x0088(0x0001)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         ByteOffset;                                        // 0x0089(0x0001)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         ByteMask;                                          // 0x008A(0x0001)(NOT AUTO-GENERATED PROPERTY)
	uint8                                         FieldMask;                                         // 0x008B(0x0001)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FBoolProperty) == 0x000008, "Wrong alignment on FBoolProperty");
static_assert(sizeof(FBoolProperty) == 0x000090, "Wrong size on FBoolProperty");
static_assert(offsetof(FBoolProperty, FieldSize) == 0x000088, "Member 'FBoolProperty::FieldSize' has a wrong offset!");
static_assert(offsetof(FBoolProperty, ByteOffset) == 0x000089, "Member 'FBoolProperty::ByteOffset' has a wrong offset!");
static_assert(offsetof(FBoolProperty, ByteMask) == 0x00008A, "Member 'FBoolProperty::ByteMask' has a wrong offset!");
static_assert(offsetof(FBoolProperty, FieldMask) == 0x00008B, "Member 'FBoolProperty::FieldMask' has a wrong offset!");

// Predefined struct FObjectPropertyBase
// 0x0008 (0x0090 - 0x0088)
class FObjectPropertyBase : public FProperty
{
public:
	class UClass*                                 PropertyClass;                                     // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FObjectPropertyBase) == 0x000008, "Wrong alignment on FObjectPropertyBase");
static_assert(sizeof(FObjectPropertyBase) == 0x000090, "Wrong size on FObjectPropertyBase");
static_assert(offsetof(FObjectPropertyBase, PropertyClass) == 0x000088, "Member 'FObjectPropertyBase::PropertyClass' has a wrong offset!");

// Predefined struct FClassProperty
// 0x0008 (0x0098 - 0x0090)
class FClassProperty final : public FObjectPropertyBase
{
public:
	class UClass*                                 MetaClass;                                         // 0x0090(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FClassProperty) == 0x000008, "Wrong alignment on FClassProperty");
static_assert(sizeof(FClassProperty) == 0x000098, "Wrong size on FClassProperty");
static_assert(offsetof(FClassProperty, MetaClass) == 0x000090, "Member 'FClassProperty::MetaClass' has a wrong offset!");

// Predefined struct FStructProperty
// 0x0008 (0x0090 - 0x0088)
class FStructProperty final : public FProperty
{
public:
	class UStruct*                                Struct;                                            // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FStructProperty) == 0x000008, "Wrong alignment on FStructProperty");
static_assert(sizeof(FStructProperty) == 0x000090, "Wrong size on FStructProperty");
static_assert(offsetof(FStructProperty, Struct) == 0x000088, "Member 'FStructProperty::Struct' has a wrong offset!");

// Predefined struct FArrayProperty
// 0x0008 (0x0090 - 0x0088)
class FArrayProperty final : public FProperty
{
public:
	class FProperty*                              InnerProperty;                                     // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FArrayProperty) == 0x000008, "Wrong alignment on FArrayProperty");
static_assert(sizeof(FArrayProperty) == 0x000090, "Wrong size on FArrayProperty");
static_assert(offsetof(FArrayProperty, InnerProperty) == 0x000088, "Member 'FArrayProperty::InnerProperty' has a wrong offset!");

// Predefined struct FDelegateProperty
// 0x0008 (0x0090 - 0x0088)
class FDelegateProperty final : public FProperty
{
public:
	class UFunction*                              SignatureFunction;                                 // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FDelegateProperty) == 0x000008, "Wrong alignment on FDelegateProperty");
static_assert(sizeof(FDelegateProperty) == 0x000090, "Wrong size on FDelegateProperty");
static_assert(offsetof(FDelegateProperty, SignatureFunction) == 0x000088, "Member 'FDelegateProperty::SignatureFunction' has a wrong offset!");

// Predefined struct FMapProperty
// 0x0010 (0x0098 - 0x0088)
class FMapProperty final : public FProperty
{
public:
	class FProperty*                              KeyProperty;                                       // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
	class FProperty*                              ValueProperty;                                     // 0x0090(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FMapProperty) == 0x000008, "Wrong alignment on FMapProperty");
static_assert(sizeof(FMapProperty) == 0x000098, "Wrong size on FMapProperty");
static_assert(offsetof(FMapProperty, KeyProperty) == 0x000088, "Member 'FMapProperty::KeyProperty' has a wrong offset!");
static_assert(offsetof(FMapProperty, ValueProperty) == 0x000090, "Member 'FMapProperty::ValueProperty' has a wrong offset!");

// Predefined struct FSetProperty
// 0x0008 (0x0090 - 0x0088)
class FSetProperty final : public FProperty
{
public:
	class FProperty*                              ElementProperty;                                   // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FSetProperty) == 0x000008, "Wrong alignment on FSetProperty");
static_assert(sizeof(FSetProperty) == 0x000090, "Wrong size on FSetProperty");
static_assert(offsetof(FSetProperty, ElementProperty) == 0x000088, "Member 'FSetProperty::ElementProperty' has a wrong offset!");

// Predefined struct FEnumProperty
// 0x0010 (0x0098 - 0x0088)
class FEnumProperty final : public FProperty
{
public:
	class FProperty*                              UnderlayingProperty;                               // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
	class UEnum*                                  Enum;                                              // 0x0090(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FEnumProperty) == 0x000008, "Wrong alignment on FEnumProperty");
static_assert(sizeof(FEnumProperty) == 0x000098, "Wrong size on FEnumProperty");
static_assert(offsetof(FEnumProperty, UnderlayingProperty) == 0x000088, "Member 'FEnumProperty::UnderlayingProperty' has a wrong offset!");
static_assert(offsetof(FEnumProperty, Enum) == 0x000090, "Member 'FEnumProperty::Enum' has a wrong offset!");

// Predefined struct FFieldPathProperty
// 0x0008 (0x0090 - 0x0088)
class FFieldPathProperty final : public FProperty
{
public:
	class FFieldClass*                            FieldClass;                                        // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FFieldPathProperty) == 0x000008, "Wrong alignment on FFieldPathProperty");
static_assert(sizeof(FFieldPathProperty) == 0x000090, "Wrong size on FFieldPathProperty");
static_assert(offsetof(FFieldPathProperty, FieldClass) == 0x000088, "Member 'FFieldPathProperty::FieldClass' has a wrong offset!");

// Predefined struct FOptionalProperty
// 0x0008 (0x0090 - 0x0088)
class FOptionalProperty final : public FProperty
{
public:
	class FProperty*                              ValueProperty;                                     // 0x0088(0x0008)(NOT AUTO-GENERATED PROPERTY)
};
static_assert(alignof(FOptionalProperty) == 0x000008, "Wrong alignment on FOptionalProperty");
static_assert(sizeof(FOptionalProperty) == 0x000090, "Wrong size on FOptionalProperty");
static_assert(offsetof(FOptionalProperty, ValueProperty) == 0x000088, "Member 'FOptionalProperty::ValueProperty' has a wrong offset!");

namespace CyclicDependencyFixupImpl
{

/*
* A wrapper for a Byte-Array of padding, that allows for casting to the actual underlaiyng type. Used for undefined structs in cylic headers.
*/
template<typename UnderlayingStructType, int32 Size, int32 Align>
struct alignas(Align) TCylicStructFixup
{
private:
	uint8 Pad[Size];

public:
	      UnderlayingStructType& GetTyped()       { return reinterpret_cast<      UnderlayingStructType&>(*this); }
	const UnderlayingStructType& GetTyped() const { return reinterpret_cast<const UnderlayingStructType&>(*this); }
};

/*
* A wrapper for a Byte-Array of padding, that inherited from UObject allows for casting to the actual underlaiyng type and access to basic UObject functionality. For cyclic classes.
*/
template<typename UnderlayingClassType, int32 Size, int32 Align = 0x8, class BaseClassType = class UObject>
struct alignas(Align) TCyclicClassFixup : public BaseClassType
{
private:
	uint8 Pad[Size];

public:
	UnderlayingClassType*       GetTyped()       { return reinterpret_cast<      UnderlayingClassType*>(this); }
	const UnderlayingClassType* GetTyped() const { return reinterpret_cast<const UnderlayingClassType*>(this); }
};

}

}
