#include "Prober.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "UE/UEGameProfile.hpp"
#include "UE/UEWrappers.hpp"
#include "Utils/Logger.hpp"

using namespace UEMemory;

namespace
{
    // Flag bits that are identical from UE 4.18 to 5.x.
    constexpr uint64_t kClassClassCastFlags = 0x29;  // CASTCLASS_UField | CASTCLASS_UStruct | CASTCLASS_UClass
    constexpr uint64_t kPackageCastFlags = 0x400000000;  // CASTCLASS_UPackage
    constexpr uint32_t FUNC_Final = 0x1, FUNC_Native = 0x400, FUNC_Event = 0x800, FUNC_MulticastDelegate = 0x10000,
                       FUNC_Delegate = 0x100000, FUNC_HasDefaults = 0x800000, FUNC_BlueprintEvent = 0x8000000,
                       FUNC_BlueprintPure = 0x10000000;
    constexpr uint64_t CPF_Parm = 0x80, CPF_OutParm = 0x100, CPF_ReturnParm = 0x400;
    // FBoolProperty::SetBoolSize for a native bool: FieldSize 1, ByteOffset 0, ByteMask 1, FieldMask 0xFF.
    constexpr uint32_t kNativeBoolQuartet = 0xFF010001;

    template <typename T>
    T Read(uintptr_t address)
    {
        T value{};
        vm_rpm_ptr(reinterpret_cast<const void *>(address), &value, sizeof(T));
        return value;
    }

    uintptr_t ReadPtr(uintptr_t address) { return Read<uintptr_t>(address); }

    uintptr_t Align8(uintptr_t value) { return (value + 7) & ~uintptr_t(7); }

    int32_t AlignUp(int32_t value, int32_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

    struct Layout
    {
        int32_t Size, Alignment;
    };

    // FStructBuilder: each member at the next offset its alignment allows, the whole
    // padded to the largest alignment.
    struct StructBuilder
    {
        int32_t End = 0, Alignment = 1;

        int32_t Add(Layout member)
        {
            const int32_t offset = AlignUp(End, member.Alignment);
            End = offset + member.Size;
            Alignment = std::max(Alignment, member.Alignment);
            return offset;
        }

        Layout Get() const { return {AlignUp(End, Alignment), Alignment}; }
    };

    // FScriptSet::GetScriptLayout: HashNextIdOffset, HashIndexOffset and Size of the set
    // element, then the Alignment and Size of its FScriptSparseArrayLayout, which an
    // FFreeListLink of two int32 bounds from below.
    std::vector<int32_t> ScriptSetLayout(Layout element)
    {
        StructBuilder setElement;
        setElement.Add(element);
        const int32_t hashNextId = setElement.Add({4, 4});  // FSetElementId
        const int32_t hashIndex = setElement.Add({4, 4});
        const Layout set = setElement.Get();
        return {hashNextId, hashIndex, set.Size, std::max(set.Alignment, 4), std::max(set.Size, 8)};
    }

    // FScriptMap::GetScriptLayout: ValueOffset, then the set layout of the key/value pair.
    std::vector<int32_t> ScriptMapLayout(Layout key, Layout value)
    {
        StructBuilder pair;
        pair.Add(key);
        const int32_t valueOffset = pair.Add(value);
        std::vector<int32_t> layout = ScriptSetLayout(pair.Get());
        layout.insert(layout.begin(), valueOffset);
        return layout;
    }

    // FName comparisons ignore case.
    bool Eq(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
        {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    }

    bool EndsWith(std::string_view s, std::string_view suffix)
    {
        return s.size() >= suffix.size() && Eq(s.substr(s.size() - suffix.size()), suffix);
    }

    // First offset in [begin, end) that satisfies match.
    std::optional<uintptr_t> Scan(uintptr_t begin, uintptr_t end, uintptr_t step, const std::function<bool(uintptr_t)> &match)
    {
        for (uintptr_t off = begin; off < end; off += step)
        {
            if (match(off)) return off;
        }
        return std::nullopt;
    }

    std::optional<uintptr_t> Plus(std::optional<uintptr_t> value, uintptr_t delta)
    {
        return value ? std::optional(*value + delta) : std::nullopt;
    }

    void Set(const char *field, uintptr_t &value, std::optional<uintptr_t> probed)
    {
        if (!probed)
            LOGW("%-30s 0x%lX (profile, not found)", field, (unsigned long)value);
        else if (*probed == value)
            LOGI("%-30s 0x%lX", field, (unsigned long)value);
        else
            LOGI("%-30s 0x%lX (profile 0x%lX)", field, (unsigned long)*probed, (unsigned long)value);

        if (probed) value = *probed;
    }

    uintptr_t Object(int32_t index) { return reinterpret_cast<uintptr_t>(UEWrappers::GetObjects()->GetObjectPtr(index)); }

    int32_t ObjectCount() { return UEWrappers::GetObjects()->GetNumElements(); }

    // Every offset is found from objects that exist in any UE 4.18+ build: the
    // CoreUObject intrinsics, KismetSystemLibrary and Actor functions and their
    // parameters, each with a value the engine fixes.
    class Prober
    {
    public:
        explicit Prober(IGameProfile *profile) : o(*profile->GetOffsets())
        {
            auto elf = profile->GetUnrealELF();
            elfBase = elf.base();
            elfEnd = elf.end();
        }

        void Run()
        {
            if (!ProbeObject() || !ProbeStructs()) return;
            if (!usesUProperty) ProbeProperties();
            ProbeEnumNames();
        }

    private:
        UE_Offsets &o;
        uintptr_t elfBase = 0, elfEnd = 0;

        uintptr_t package = 0, objectClass = 0, classClass = 0, packageClass = 0;
        uintptr_t structClass = 0, scriptStructClass = 0, functionClass = 0, enumClass = 0;
        uintptr_t kismet = 0, actor = 0, vector = 0, defaultObject = 0, guid = 0, transform = 0;
        uintptr_t ubergraph = 0, isValid = 0, printString = 0, beginPlay = 0, tick = 0, getLocation = 0;
        bool usesUProperty = false;

        std::string NameAt(uintptr_t address) const
        {
            if (!address) return {};
            const uintptr_t idOffset = o.Config.isUsingCasePreservingName ? o.FName.DisplayIndex : o.FName.ComparisonIndex;
            int32_t id = -1;
            if (!vm_rpm_ptr(reinterpret_cast<const void *>(address + idOffset), &id, sizeof(id)) || id < 0)
                return {};
            if (!o.Config.isUsingOutlineNumberName && Read<uint32_t>(address + o.FName.Number) > 0xFFFF)
                return {};
            return UEWrappers::GetUEVars()->GetNameByID(id);
        }

        bool InLibrary(uintptr_t address) const { return address >= elfBase && address < elfEnd; }

        std::string ObjectName(uintptr_t object) const { return object ? NameAt(object + o.UObject.NamePrivate) : ""; }
        uintptr_t ClassOf(uintptr_t object) const { return object ? ReadPtr(object + o.UObject.ClassPrivate) : 0; }
        std::string FieldName(uintptr_t field) const { return field ? NameAt(field + o.FField.NamePrivate) : ""; }

        std::string FieldClassName(uintptr_t field) const
        {
            const uintptr_t fieldClass = field ? ReadPtr(field + o.FField.ClassPrivate) : 0;
            return fieldClass ? NameAt(fieldClass + o.FFieldClass.Name) : "";
        }

        bool IsProperty(uintptr_t field) const { return EndsWith(FieldClassName(field), "Property"); }

        uintptr_t Function(uintptr_t structure, std::string_view name) const
        {
            uintptr_t field = structure ? ReadPtr(structure + o.UStruct.Children) : 0;
            for (int i = 0; field && i < 4096; ++i, field = ReadPtr(field + o.UField.Next))
            {
                if (Eq(ObjectName(field), name)) return field;
            }
            return 0;
        }

        // Objects by (class name, object name), found in one pass over the object array.
        std::vector<uintptr_t> FindObjects(std::initializer_list<std::pair<std::string_view, std::string_view>> wanted) const
        {
            std::vector<uintptr_t> found(wanted.size(), 0);
            size_t remaining = wanted.size();
            for (int32_t i = 0, count = ObjectCount(); i < count && remaining; ++i)
            {
                const uintptr_t object = Object(i);
                const std::string name = ObjectName(object);
                size_t k = 0;
                for (const auto &[className, objectName] : wanted)
                {
                    if (!found[k] && Eq(name, objectName) && Eq(ObjectName(ClassOf(object)), className))
                    {
                        found[k] = object;
                        --remaining;
                    }
                    ++k;
                }
            }
            return found;
        }

        // Objects 0 and 1 are the /Script/CoreUObject package and the Object class.
        bool ProbeObject()
        {
            package = Object(0);
            objectClass = Object(1);
            if (!package || !objectClass)
            {
                LOGE("Probe: the object array has no CoreUObject objects");
                return false;
            }

            Set("UObject::InternalIndex", o.UObject.InternalIndex, Scan(8, 0x40, 4, [&](uintptr_t off)
            {
                for (int32_t i = 1; i < 8; ++i)
                {
                    if (Read<int32_t>(Object(i) + off) != i) return false;
                }
                return true;
            }));
            Set("UObject::NamePrivate", o.UObject.NamePrivate, Scan(8, 0x40, 4, [&](uintptr_t off)
            {
                return Eq(NameAt(objectClass + off), "Object");
            }));
            Set("UObject::ClassPrivate", o.UObject.ClassPrivate, Scan(8, 0x40, 8, [&](uintptr_t off)
            {
                return Eq(ObjectName(ReadPtr(objectClass + off)), "Class") && Eq(ObjectName(ReadPtr(package + off)), "Package");
            }));
            Set("UObject::OuterPrivate", o.UObject.OuterPrivate, Scan(8, 0x40, 8, [&](uintptr_t off)
            {
                return ReadPtr(objectClass + off) == package && ReadPtr(package + off) == 0;
            }));
            Set("UObject::ObjectFlags", o.UObject.ObjectFlags, Scan(8, 0x40, 4, [&](uintptr_t off)
            {
                auto overlaps = [&](uintptr_t field, uintptr_t size) { return off + 4 > field && off < field + size; };
                if (overlaps(o.UObject.InternalIndex, 4) || overlaps(o.UObject.NamePrivate, o.FName.Size) ||
                    overlaps(o.UObject.ClassPrivate, 8) || overlaps(o.UObject.OuterPrivate, 8))
                    return false;
                // The CoreUObject package carries only RF_Public.
                if (Read<uint32_t>(package + off) != 1) return false;
                for (int32_t i = 1; i < 4; ++i)
                {
                    const uint32_t flags = Read<uint32_t>(Object(i) + off);
                    if (!flags || flags > 0x03FFFFFF) return false;
                }
                return true;
            }));
            return true;
        }

        bool ProbeStructs()
        {
            classClass = ClassOf(objectClass);
            packageClass = ClassOf(package);
            const auto found = FindObjects({{"Class", "Struct"}, {"Class", "ScriptStruct"}, {"Class", "Function"},
                                            {"Class", "Enum"}, {"Class", "KismetSystemLibrary"}, {"Class", "Actor"},
                                            {"ScriptStruct", "Vector"}, {"Object", "Default__Object"},
                                            {"ScriptStruct", "Guid"}, {"ScriptStruct", "Transform"}});
            structClass = found[0], scriptStructClass = found[1], functionClass = found[2], enumClass = found[3];
            kismet = found[4], actor = found[5], vector = found[6], defaultObject = found[7];
            guid = found[8], transform = found[9];
            if (!structClass || !functionClass || !kismet || !actor)
            {
                LOGE("Probe: Struct, Function, KismetSystemLibrary or Actor not found; later offsets stay as profiled");
                return false;
            }

            // UField adds only Next to UObject, so the link that chains the functions
            // of KismetSystemLibrary sits at sizeof(UObject).
            uintptr_t objectEnd = 0;
            for (auto [field, size] : {std::pair{o.UObject.InternalIndex, uintptr_t(4)}, {o.UObject.ObjectFlags, 4},
                                       {o.UObject.NamePrivate, o.FName.Size}, {o.UObject.ClassPrivate, 8}, {o.UObject.OuterPrivate, 8}})
                objectEnd = std::max(objectEnd, field + size);
            std::optional<uintptr_t> next;
            int longest = 3;
            for (uintptr_t children = Align8(objectEnd); children < Align8(objectEnd) + 0x40; children += 8)
            {
                const uintptr_t head = ReadPtr(kismet + children);
                if (!head || ClassOf(head) != functionClass) continue;
                for (uintptr_t link = Align8(objectEnd); link < Align8(objectEnd) + 0x40; link += 8)
                {
                    int chain = 1;
                    for (uintptr_t f = ReadPtr(head + link); chain < 64 && f && ClassOf(f) == functionClass; f = ReadPtr(f + link))
                        ++chain;
                    if (chain > longest)
                    {
                        longest = chain;
                        next = link;
                    }
                }
            }
            Set("UField::Next", o.UField.Next, next);
            const uintptr_t objectSize = o.UField.Next;

            // The Object class is exactly as large as UObject; Struct is larger.
            Set("UStruct::PropertiesSize", o.UStruct.PropertiesSize, Scan(objectSize, objectSize + 0x40, 4, [&](uintptr_t off)
            {
                const int32_t structSize = Read<int32_t>(structClass + off);
                return Read<int32_t>(objectClass + off) == int32_t(objectSize) && structSize > int32_t(objectSize) && structSize < 0x400;
            }));
            const uintptr_t structSize = Read<uint32_t>(structClass + o.UStruct.PropertiesSize);
            const uintptr_t classSize = Read<uint32_t>(classClass + o.UStruct.PropertiesSize);
            const uintptr_t functionSize = Read<uint32_t>(functionClass + o.UStruct.PropertiesSize);

            Set("UStruct::SuperStruct", o.UStruct.SuperStruct, Scan(objectSize, structSize, 8, [&](uintptr_t off)
            {
                return ReadPtr(packageClass + off) == objectClass && ReadPtr(classClass + off) == structClass;
            }));
            Set("UStruct::Children", o.UStruct.Children, Scan(objectSize, structSize, 8, [&](uintptr_t off)
            {
                return ClassOf(ReadPtr(kismet + off)) == functionClass && ClassOf(ReadPtr(objectClass + off)) == functionClass;
            }));
            // FGuid is four int32 and FTransform is 16-aligned in every version; the Object
            // class is aligned as UObject. Functions are not used: a build may leave their
            // MinAlignment at 1 whatever their parameters.
            Set("UStruct::MinAlignment", o.UStruct.MinAlignment, Scan(objectSize, structSize, 4, [&](uintptr_t off)
            {
                return guid && transform && off != o.UStruct.PropertiesSize && Read<int32_t>(objectClass + off) == 8 &&
                       Read<int32_t>(guid + off) == 4 && Read<int32_t>(transform + off) == 16;
            }));

            // Before UE 4.25 properties are UObjects linked through Children, so
            // ExecuteUbergraph's only parameter is its first child.
            ubergraph = Function(objectClass, "ExecuteUbergraph");
            usesUProperty = ubergraph && Eq(ObjectName(ReadPtr(ubergraph + o.UStruct.Children)), "EntryPoint");
            if (usesUProperty)
            {
                LOGI("Probe: properties are UObjects; FField and FProperty offsets stay as profiled");
                o.UStruct.ChildProperties = 0;
            }
            else
            {
                Set("UStruct::ChildProperties", o.UStruct.ChildProperties, Scan(objectSize, structSize, 8, [&](uintptr_t off)
                {
                    const uintptr_t field = off != o.UStruct.Children && ubergraph ? ReadPtr(ubergraph + off) : 0;
                    return field && Scan(8, 0x40, 4, [&](uintptr_t name) { return Eq(NameAt(field + name), "EntryPoint"); });
                }));
            }

            Set("UClass::CastFlags", o.UClass.CastFlags, Scan(structSize, classSize, 8, [&](uintptr_t off)
            {
                return Read<uint64_t>(classClass + off) == kClassClassCastFlags && Read<uint64_t>(packageClass + off) == kPackageCastFlags;
            }));
            Set("UClass::DefaultObject", o.UClass.DefaultObject, Scan(structSize, classSize, 8, [&](uintptr_t off)
            {
                return defaultObject && ReadPtr(objectClass + off) == defaultObject;
            }));

            isValid = Function(kismet, "IsValid");
            printString = Function(kismet, "PrintString");
            beginPlay = Function(actor, "ReceiveBeginPlay");
            tick = Function(actor, "ReceiveTick");
            getLocation = Function(actor, "K2_GetActorLocation");
            if (!isValid || !printString || !beginPlay || !tick || !getLocation)
            {
                LOGE("Probe: Kismet or Actor functions not found; later offsets stay as profiled");
                return false;
            }

            Set("UFunction::FunctionFlags", o.UFunction.EFunctionFlags, Scan(structSize, functionSize, 4, [&](uintptr_t off)
            {
                auto has = [&](uintptr_t f, uint32_t flags) { return (Read<uint32_t>(f + off) & flags) == flags; };
                auto isEvent = [&](uintptr_t f) { return has(f, FUNC_Event | FUNC_BlueprintEvent) && !has(f, FUNC_Native); };
                return has(isValid, FUNC_Native | FUNC_Final | FUNC_BlueprintPure) &&
                       has(getLocation, FUNC_Native | FUNC_Final | FUNC_BlueprintPure) &&
                       has(printString, FUNC_Native | FUNC_Final | FUNC_HasDefaults) && !has(printString, FUNC_BlueprintPure) &&
                       isEvent(beginPlay) && isEvent(tick);
            }));
            auto outsideFlags = [&](uintptr_t off) { return off < o.UFunction.EFunctionFlags || off >= o.UFunction.EFunctionFlags + 4; };
            Set("UFunction::NumParms", o.UFunction.NumParams, Scan(structSize, functionSize, 1, [&](uintptr_t off)
            {
                return outsideFlags(off) && Read<uint8_t>(beginPlay + off) == 0 && Read<uint8_t>(tick + off) == 1 &&
                       Read<uint8_t>(getLocation + off) == 1 && Read<uint8_t>(isValid + off) == 2;
            }));
            Set("UFunction::ParmsSize", o.UFunction.ParamSize, Scan(structSize, functionSize, 2, [&](uintptr_t off)
            {
                return outsideFlags(off) && Read<uint16_t>(beginPlay + off) == 0 &&
                       Read<uint16_t>(tick + off) == 4 && Read<uint16_t>(isValid + off) == 9;
            }));
            // Native functions point into the library; script events share ProcessInternal.
            Set("UFunction::Func", o.UFunction.Func, Scan(structSize, functionSize, 8, [&](uintptr_t off)
            {
                const uintptr_t a = ReadPtr(isValid + off), b = ReadPtr(printString + off), c = ReadPtr(getLocation + off);
                const uintptr_t script = ReadPtr(beginPlay + off);
                return InLibrary(a) && InLibrary(b) && InLibrary(c) && a != b && b != c && a != c &&
                       script && script == ReadPtr(tick + off);
            }));
            return true;
        }

        void ProbeProperties()
        {
            const uintptr_t entryPoint = ubergraph ? ReadPtr(ubergraph + o.UStruct.ChildProperties) : 0;  // int32 EntryPoint
            const uintptr_t deltaSeconds = ReadPtr(tick + o.UStruct.ChildProperties);                     // float DeltaSeconds
            const uintptr_t object = ReadPtr(isValid + o.UStruct.ChildProperties);                        // UObject* Object
            const uintptr_t location = ReadPtr(getLocation + o.UStruct.ChildProperties);                  // FVector ReturnValue
            if (!entryPoint || !deltaSeconds || !object || !location)
            {
                LOGE("Probe: function parameters not found; FField and FProperty offsets stay as profiled");
                return;
            }

            Set("FField::NamePrivate", o.FField.NamePrivate, Scan(8, 0x40, 4, [&](uintptr_t off)
            {
                return Eq(NameAt(entryPoint + off), "EntryPoint") && Eq(NameAt(deltaSeconds + off), "DeltaSeconds");
            }));
            Set("FField::Owner", o.FField.Owner, Scan(8, 0x40, 8, [&](uintptr_t off)
            {
                return ReadPtr(entryPoint + off) == ubergraph && ReadPtr(deltaSeconds + off) == tick;
            }));
            Set("FField::Next", o.FField.Next, Scan(8, 0x40, 8, [&](uintptr_t off)
            {
                const uintptr_t second = ReadPtr(object + off);
                return ReadPtr(entryPoint + off) == 0 && Eq(FieldName(second), "ReturnValue") && ReadPtr(second + off) == 0;
            }));
            const uintptr_t returnValue = ReadPtr(object + o.FField.Next);  // bool ReturnValue
            Set("FField::ClassPrivate", o.FField.ClassPrivate, Scan(8, 0x40, 8, [&](uintptr_t off)
            {
                return Eq(NameAt(ReadPtr(entryPoint + off) + o.FFieldClass.Name), "IntProperty") &&
                       Eq(NameAt(ReadPtr(deltaSeconds + off) + o.FFieldClass.Name), "FloatProperty");
            }));
            // FField declares FlagsPrivate right after NamePrivate.
            Set("FField::FlagsPrivate", o.FField.FlagsPrivate, o.FField.NamePrivate + o.FName.Size);

            const auto arrayDim = Scan(o.FField.FlagsPrivate + 4, o.FField.FlagsPrivate + 0x40, 4, [&](uintptr_t off)
            {
                for (auto [property, size] : {std::pair{entryPoint, 4}, {deltaSeconds, 4}, {object, 8}, {returnValue, 1}})
                {
                    if (Read<int32_t>(property + off) != 1 || Read<int32_t>(property + off + 4) != size) return false;
                }
                return true;
            });
            Set("FProperty::ArrayDim", o.FProperty.ArrayDim, arrayDim);
            Set("FProperty::ElementSize", o.FProperty.ElementSize, Plus(arrayDim, 4));
            const uint64_t returned = CPF_Parm | CPF_OutParm | CPF_ReturnParm;
            Set("FProperty::PropertyFlags", o.FProperty.PropertyFlags, Scan(Align8(o.FProperty.ElementSize + 4), o.FProperty.ElementSize + 0x40, 8, [&](uintptr_t off)
            {
                auto flags = [&](uintptr_t property) { return Read<uint64_t>(property + off); };
                return (flags(entryPoint) & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm &&
                       (flags(object) & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm &&
                       (flags(returnValue) & returned) == returned && (flags(location) & returned) == returned;
            }));
            Set("FProperty::Offset_Internal", o.FProperty.Offset_Internal, Scan(o.FProperty.PropertyFlags + 8, o.FProperty.PropertyFlags + 0x28, 4, [&](uintptr_t off)
            {
                return Read<int32_t>(entryPoint + off) == 0 && Read<int32_t>(deltaSeconds + off) == 0 &&
                       Read<int32_t>(object + off) == 0 && Read<int32_t>(returnValue + off) == 8;
            }));

            // FFieldClass::ConstructFn follows SuperClass and DefaultObject; it is the first
            // slot after SuperClass that holds a function in the library.
            const uintptr_t intClass = ReadPtr(entryPoint + o.FField.ClassPrivate);
            const uintptr_t floatClass = ReadPtr(deltaSeconds + o.FField.ClassPrivate);
            Set("FFieldClass::ConstructFn", o.FFieldClass.ConstructFn, Scan(o.FFieldClass.SuperClass + 8, o.FFieldClass.SuperClass + 0x28, 8, [&](uintptr_t off)
            {
                const uintptr_t a = ReadPtr(intClass + off), b = ReadPtr(floatClass + off);
                return InLibrary(a) && InLibrary(b) && a != b;
            }));

            // FIntProperty declares no members of its own.
            const uintptr_t profiledSize = o.FProperty.Size;
            const uint32_t intSize = UE_FFieldClass(reinterpret_cast<uint8_t *>(intClass)).GetInstanceSize();
            Set("sizeof(FProperty)", o.FProperty.Size, intSize ? std::optional<uintptr_t>(intSize) : std::nullopt);
            const uintptr_t size = o.FProperty.Size;

            // FBoolProperty::FieldSize is the first member FBoolProperty declares, which
            // some builds place after a leading byte.
            auto distance = [&](uintptr_t off) { return off > size ? off - size : size - off; };
            std::optional<uintptr_t> fieldSize;
            for (uintptr_t off = size - 16; off < size + 8; ++off)
            {
                if (Read<uint32_t>(returnValue + off) == kNativeBoolQuartet && (!fieldSize || distance(off) < distance(*fieldSize)))
                    fieldSize = off;
            }
            Set("FBoolProperty::FieldSize", o.FBoolProperty.FieldSize, fieldSize);

            // The profile places subclass members relative to its own sizeof(FProperty).
            for (uintptr_t *member : {&o.FStructProperty.Struct, &o.FObjectPropertyBase.PropertyClass, &o.FClassProperty.MetaClass,
                                      &o.FSoftClassProperty.MetaClass, &o.FByteProperty.Enum, &o.FInterfaceProperty.InterfaceClass,
                                      &o.FFieldPathProperty.PropertyClass, &o.FDelegateProperty.SignatureFunction,
                                      &o.FMulticastDelegateProperty.SignatureFunction, &o.FOptionalProperty.ValueProperty,
                                      &o.FEnumProperty.UnderlyingType, &o.FEnumProperty.Enum, &o.FArrayProperty.Inner,
                                      &o.FSetProperty.ElementProp, &o.FMapProperty.KeyProp, &o.FMapProperty.ValueProp})
                *member = *member - profiledSize + size;

            // Some builds put members in front of the first one an individual subclass
            // declares, so each is found on its own, within 0x20 bytes after FProperty.
            Set("FStructProperty::Struct", o.FStructProperty.Struct, Scan(size, size + 0x20, 8, [&](uintptr_t off)
            {
                return vector && ReadPtr(location + off) == vector;
            }));
            Set("FObjectPropertyBase::PropertyClass", o.FObjectPropertyBase.PropertyClass, Scan(size, size + 0x20, 8, [&](uintptr_t off)
            {
                return ReadPtr(object + off) == objectClass;
            }));

            auto properties = CollectProperties();
            // The slot from `from` at which the most sampled properties of the class point
            // to an object `is` accepts. Slots past a small subclass read the next heap
            // object, which agrees across samples far less often than the real member.
            auto member = [&](const char *cls, uintptr_t from, const std::function<bool(uintptr_t)> &is) -> std::optional<uintptr_t>
            {
                const auto &samples = properties[cls];
                std::optional<uintptr_t> best;
                size_t most = 0;
                for (uintptr_t off = from; off < from + 0x20; off += 8)
                {
                    const size_t count = std::count_if(samples.begin(), samples.end(), [&](uintptr_t p) { return is(ReadPtr(p + off)); });
                    if (count > most)
                    {
                        most = count;
                        best = off;
                    }
                }
                return best;
            };
            auto isProperty = [&](uintptr_t p) { return IsProperty(p); };
            auto isClass = [&](uintptr_t p) { return ClassOf(p) == classClass; };
            auto isSignature = [&](uint32_t flags)
            {
                return [this, flags](uintptr_t p)
                {
                    return EndsWith(ObjectName(ClassOf(p)), "Function") && (Read<uint32_t>(p + o.UFunction.EFunctionFlags) & flags) == flags;
                };
            };
            Set("FClassProperty::MetaClass", o.FClassProperty.MetaClass, member("ClassProperty", o.FObjectPropertyBase.PropertyClass + 8, isClass));
            Set("FSoftClassProperty::MetaClass", o.FSoftClassProperty.MetaClass, member("SoftClassProperty", o.FObjectPropertyBase.PropertyClass + 8, isClass));
            Set("FByteProperty::Enum", o.FByteProperty.Enum, member("ByteProperty", size, [&](uintptr_t p) { return enumClass && ClassOf(p) == enumClass; }));
            Set("FInterfaceProperty::InterfaceClass", o.FInterfaceProperty.InterfaceClass, member("InterfaceProperty", size, isClass));
            Set("FFieldPathProperty::PropertyClass", o.FFieldPathProperty.PropertyClass, member("FieldPathProperty", size, [&](uintptr_t p)
            {
                return EndsWith(NameAt(p + o.FFieldClass.Name), "Property");
            }));
            Set("FDelegateProperty::SignatureFunction", o.FDelegateProperty.SignatureFunction, member("DelegateProperty", size, isSignature(FUNC_Delegate)));
            auto multicast = member("MulticastInlineDelegateProperty", size, isSignature(FUNC_Delegate | FUNC_MulticastDelegate));
            if (!multicast) multicast = member("MulticastSparseDelegateProperty", size, isSignature(FUNC_Delegate | FUNC_MulticastDelegate));
            Set("FMulticastDelegateProperty::SignatureFunction", o.FMulticastDelegateProperty.SignatureFunction, multicast);
            Set("FOptionalProperty::ValueProperty", o.FOptionalProperty.ValueProperty, member("OptionalProperty", size, isProperty));
            Set("FArrayProperty::Inner", o.FArrayProperty.Inner, member("ArrayProperty", size, isProperty));
            Set("FEnumProperty::UnderlyingType", o.FEnumProperty.UnderlyingType, member("EnumProperty", size, isProperty));
            Set("FEnumProperty::Enum", o.FEnumProperty.Enum, member("EnumProperty", size, [&](uintptr_t p) { return enumClass && ClassOf(p) == enumClass; }));

            // FSetProperty::SetLayout follows ElementProp and FMapProperty::MapLayout follows
            // ValueProp. Both equal FScriptSet/FScriptMap::GetScriptLayout of the inner
            // properties, which also fixes the key/value order.
            auto layoutSlot = [&](const char *cls, uintptr_t pointers, const std::function<std::vector<int32_t>(uintptr_t)> &expected)
            {
                return Scan(size, size + 0x20, 8, [&](uintptr_t off)
                {
                    size_t matched = 0;
                    for (uintptr_t property : properties[cls])
                    {
                        const std::vector<int32_t> layout = expected(property + off);
                        if (layout.empty()) continue;
                        for (size_t k = 0; k < layout.size(); ++k)
                        {
                            if (Read<int32_t>(property + off + pointers * 8 + k * 4) != layout[k]) return false;
                        }
                        ++matched;
                    }
                    return matched > 0;
                });
            };
            Set("FSetProperty::ElementProp", o.FSetProperty.ElementProp, layoutSlot("SetProperty", 1, [&](uintptr_t slot) -> std::vector<int32_t>
            {
                const auto element = InnerLayout(ReadPtr(slot));
                return element ? ScriptSetLayout(*element) : std::vector<int32_t>{};
            }));
            const auto key = layoutSlot("MapProperty", 2, [&](uintptr_t slot) -> std::vector<int32_t>
            {
                const auto keyLayout = InnerLayout(ReadPtr(slot)), valueLayout = InnerLayout(ReadPtr(slot + 8));
                return keyLayout && valueLayout ? ScriptMapLayout(*keyLayout, *valueLayout) : std::vector<int32_t>{};
            });
            Set("FMapProperty::KeyProp", o.FMapProperty.KeyProp, key);
            Set("FMapProperty::ValueProp", o.FMapProperty.ValueProp, Plus(key, 8));
        }

        // Up to 256 properties of each class, by class name, from the members and
        // parameters of the classes, script structs and functions.
        std::unordered_map<std::string, std::vector<uintptr_t>> CollectProperties() const
        {
            std::unordered_map<uintptr_t, std::string> classNames;
            std::unordered_map<std::string, std::vector<uintptr_t>> properties;
            for (int32_t i = 0, count = ObjectCount(); i < count; ++i)
            {
                const uintptr_t structure = Object(i);
                const uintptr_t cls = ClassOf(structure);
                if (!cls || (cls != classClass && cls != scriptStructClass && cls != functionClass)) continue;
                uintptr_t field = ReadPtr(structure + o.UStruct.ChildProperties);
                for (int n = 0; field && n < 512; ++n, field = ReadPtr(field + o.FField.Next))
                {
                    const uintptr_t fieldClass = ReadPtr(field + o.FField.ClassPrivate);
                    auto name = classNames.find(fieldClass);
                    if (name == classNames.end())
                        name = classNames.emplace(fieldClass, fieldClass ? NameAt(fieldClass + o.FFieldClass.Name) : "").first;
                    auto &samples = properties[name->second];
                    if (samples.size() < 256) samples.push_back(field);
                }
            }
            return properties;
        }

        // Size and alignment of an inner property whose class fixes its C++ alignment.
        std::optional<Layout> InnerLayout(uintptr_t property) const
        {
            static const std::unordered_map<std::string, int32_t> kAlignments = {
                {"ByteProperty", 1}, {"Int8Property", 1}, {"Int16Property", 2}, {"UInt16Property", 2},
                {"IntProperty", 4}, {"UInt32Property", 4}, {"FloatProperty", 4}, {"NameProperty", 4},
                {"Int64Property", 8}, {"UInt64Property", 8}, {"DoubleProperty", 8}, {"StrProperty", 8},
                {"ObjectProperty", 8}, {"ClassProperty", 8}};
            if (!property || Read<int32_t>(property + o.FProperty.ArrayDim) != 1) return std::nullopt;
            const int32_t elementSize = Read<int32_t>(property + o.FProperty.ElementSize);
            const std::string name = FieldClassName(property);
            const auto known = kAlignments.find(name);
            // An enum property is as wide and as aligned as its underlying integer.
            const int32_t alignment = Eq(name, "EnumProperty") ? elementSize : known != kAlignments.end() ? known->second : 0;
            if (elementSize <= 0 || (alignment != 1 && alignment != 2 && alignment != 4 && alignment != 8)) return std::nullopt;
            return Layout{elementSize, alignment};
        }

        // UEnum::Names is a TArray<TPair<FName, int64>> whose last entry is the generated *_MAX.
        void ProbeEnumNames()
        {
            std::vector<uintptr_t> enums;
            for (int32_t i = 0, count = ObjectCount(); enumClass && i < count && enums.size() < 8; ++i)
            {
                if (ClassOf(Object(i)) == enumClass) enums.push_back(Object(i));
            }
            const uintptr_t pairSize = Align8(o.FName.Size) + sizeof(int64_t);
            auto names = [&](uintptr_t e, uintptr_t off)
            {
                const uintptr_t data = ReadPtr(e + off);
                const int32_t num = Read<int32_t>(e + off + 8), max = Read<int32_t>(e + off + 12);
                return data && num > 0 && num <= 4096 && max >= num && !NameAt(data).empty() &&
                       EndsWith(NameAt(data + (num - 1) * pairSize), "_MAX");
            };
            // Most of the sampled enums must agree, at least three when there are three.
            std::optional<uintptr_t> best;
            size_t bestCount = std::min<size_t>(3, enums.size());
            for (uintptr_t off = o.UField.Next + 8; !enums.empty() && off < o.UField.Next + 0x88; off += 8)
            {
                const size_t count = std::count_if(enums.begin(), enums.end(), [&](uintptr_t e) { return names(e, off); });
                if (count >= bestCount && (!best || count > bestCount))
                {
                    bestCount = count;
                    best = off;
                }
            }
            Set("UEnum::Names", o.UEnum.Names, best);
        }
    };
}  // namespace

void ProbeOffsets(IGameProfile *profile)
{
    LOGI("Probing reflection offsets...");
    Prober(profile).Run();
    LOGI("==========================");
}
