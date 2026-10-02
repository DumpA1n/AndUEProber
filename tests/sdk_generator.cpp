#include "SDKCoreGen.hpp"
#include "UECoreEmbed.hpp"
#include "andueprober/Export.hpp"
#include "OwnedObjectArray.hpp"
#include <thread>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const bool chunked = std::string(argv[2]) == "chunked";
    OwnedObjectArray memory(chunked);
    if (chunked) {
        for (std::uint32_t index = 0; index < 4; ++index) {
            memory.put<std::uint32_t>(1024 + index * 64, 0x5a5a5a5a);
            memory.put<std::uint32_t>(1024 + index * 64 + 12, index);
        }
    }
    andueprober::Snapshot snapshot;
    snapshot.sessionId = argv[2];
    snapshot.moduleIdentity = "owned-sdk-array-fixture-v1";
    snapshot.layout = andueprober::Layout::UProperty;
    snapshot.layoutIdentity = andueprober::objectArrayLayoutIdentity(memory.profile);
    snapshot.generation = memory.generation();
    andueprober::Session session(snapshot);
    if (!session.start([&](auto& result, const auto& cancelled) {
        andueprober::ReadBudget budget;
        budget.generation = memory.generation(); budget.cancelled = &cancelled;
        std::vector<andueprober::Offset> values;
        auto status = andueprober::probeObjectArrayIndices(memory, memory.Array, memory.profile, 32, budget, values);
        if (!status) return status;
        if (values.size() != 1 || values[0].validation != andueprober::Validation::Validated)
            return andueprober::Status{andueprober::Error::InvalidEvidence, "Owned object index fixture is not uniquely validated"};
        return andueprober::publishOffset(result, "UObject::InternalIndex", std::move(values[0]));
    })) return 3;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session.snapshot()->state == andueprober::TaskState::Running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    if (!session.stop()) return 4;
    const auto frozen = session.snapshot();
    if (frozen->state != andueprober::TaskState::Succeeded || !andueprober::validateSnapshot(*frozen)) return 4;
    const auto indexOffset = *frozen->offsets.at("UObject::InternalIndex").value;
    sdkcoregen::UObjectArrayLayout layout;
    layout.ItemStride = *memory.profile.itemStride;
    layout.ObjectOffset = *memory.profile.itemObject;
    layout.NumElementsPerChunk = memory.profile.elementsPerChunk;
    layout.ObjObjectsOffset = 0;
    layout.ObjectsOffset = *memory.profile.objects;
    layout.NumElementsOffset = *memory.profile.count;
    layout.MaxElementsOffset = *memory.profile.capacity;
    layout.NumChunksOffset = *memory.profile.chunkCount;
    layout.MaxChunksOffset = *memory.profile.chunkCapacity;
    std::string header = "#pragma once\n#include <cstdint>\n#include <cstring>\n#include <string>\n#include <functional>\n#include <type_traits>\n#include <cstddef>\n#include \"UEAssert.h\"\nusing int32 = int32_t; using uint8 = uint8_t; using int64 = int64_t;\nnamespace SDK { class UObject;\n";
    header += sdkcoregen::GenUObjectArray(layout);
    // The chunked fixture reads display strings through ITextData's vtable as Delta
    // Force does; the flat one reads the stock embedded source string.
    header += "class FString { public: const char* Data = \"\"; std::string ToString() const { return Data; } };\n";
    header += sdkcoregen::GenFText(chunked ? sdkcoregen::FTextLayout{0x28, 3} : sdkcoregen::FTextLayout{});
    header += "class UObject { public: ";
    if (indexOffset) header += "uint8 Padding[" + std::to_string(indexOffset) + "]; ";
    header += "int32 InternalIndex; };\nstatic_assert(offsetof(UObject, InternalIndex) == " +
        std::to_string(indexOffset) + ");\n}\n";

    std::string runtime = R"CPP(#include "SDK.hpp"
)CPP";
    if (chunked) runtime += R"CPP(
static SDK::FString displayString;
static const SDK::FString& Display(const SDK::FTextImpl::FTextData*) { return displayString; }
)CPP";
    runtime += R"CPP(
int main() {
    SDK::UObject objects[4]{};
    SDK::FUObjectItem items[4]{};
    for (int32 i = 0; i < 4; ++i) { objects[i].InternalIndex = i; items[i].Object = &objects[i]; }
    SDK::FUObjectArray array{};
    array.ObjObjects.NumElements = array.ObjObjects.MaxElements = 4;
)CPP";
    runtime += chunked ?
        "SDK::FUObjectItem* chunks[2] = {items, items + 2}; array.ObjObjects.Objects = chunks; array.ObjObjects.NumChunks = array.ObjObjects.MaxChunks = 2;\n" :
        "array.ObjObjects.Objects = items;\n";
    runtime += R"CPP(
    SDK::FText text{};
    if (text.IsValid() || !text.ToString().empty() || !text.GetStringRef().ToString().empty()) return 3;
)CPP";
    runtime += chunked ? R"CPP(
    void* vtable[4] = {nullptr, nullptr, nullptr, reinterpret_cast<void*>(&Display)};
    SDK::FTextImpl::FTextData data{vtable};
    displayString.Data = "display";
    text.TextData = &data;
    if (text.ToString() != "display" || &text.GetStringRef() != &displayString) return 4;
)CPP" : R"CPP(
    SDK::FTextImpl::FTextData data{};
    data.TextSource.Data = "source";
    text.TextData = &data;
    if (offsetof(SDK::FTextImpl::FTextData, TextSource) != 0x28 || text.ToString() != "source" ||
        &text.GetStringRef() != &data.TextSource) return 4;
)CPP";
    runtime += R"CPP(
    if (array.GetObjectArrayNum() != 4 || array.IndexToObject(-1) || array.IndexToObject(4)) return 1;
    for (int32 i = 0; i < 4; ++i) {
        if (array.IndexToObject(i)->Object != &objects[i] || array.ObjectToIndex(&objects[i]) != i) return 2;
    }
    return 0;
}
)CPP";
    auto result = andueprober::publishExport(*frozen, {
        {"SDK_A/OwnedSmoke.cpp", runtime}, {"SDK_A/SDK.hpp", header}, {"SDK_A/UEAssert.h", kUECoreUEAssertH},
        {"SDK_A/Basic.cpp", "#include \"SDK.hpp\"\nSDK::FUObjectArray* SDK::GUObjectArray = nullptr;\nint SDK::FUObjectArray::ObjectToIndex(const SDK::UObject* object) const { return object->InternalIndex; }\n"}
    }, {std::filesystem::absolute(argv[1])});
    return result.status ? 0 : 1;
}
