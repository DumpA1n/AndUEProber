#include "andueprober/Names.hpp"
#include "andueprober/EngineModel.hpp"
#include <array>
#include <cstring>
#include <limits>
#include <set>

namespace andueprober {
namespace {
Status addressOf(std::uintptr_t base, std::uint64_t offset, std::uintptr_t& result) {
    if (offset > std::numeric_limits<std::uintptr_t>::max() - base)
        return {Error::Overflow, "Name address overflow"};
    result = base + offset;
    return {};
}
Status checkProfile(const NamePoolProfile& profile) {
    if (profile.outlineNumbers) return {Error::Unsupported, "Outline-number pool entries are unsupported"};
    if (profile.identity.empty() || !profile.blocks || !profile.header || !profile.string ||
        profile.blockBits < 8 || profile.blockBits > 20 || (profile.stride != 2 && profile.stride != 4) ||
        !profile.maximumBlocks || profile.maximumBlocks > 8192 || !profile.maximumUnits || profile.maximumUnits > 1024 ||
        (profile.narrowEncoding != NarrowEncoding::Ascii && profile.narrowEncoding != NarrowEncoding::Utf8) ||
        profile.lengthShift < 1 || profile.lengthShift > 15 || *profile.blocks > 4096 ||
        *profile.header > 8 || *profile.string != *profile.header + sizeof(std::uint16_t))
        return {Error::InvalidArgument, "Incomplete or out-of-range name pool profile"};
    return {};
}
Status checkLayout(const NameLayout& layout) {
    if (!layout.comparison || layout.size < 4 || layout.size > 16)
        return {Error::InvalidArgument, "Incomplete FName layout"};
    std::set<std::uint32_t> fields;
    for (auto offset : {layout.comparison, layout.display, layout.number}) {
        if (!offset) continue;
        if (*offset > layout.size - 4 || *offset % 4 || !fields.insert(*offset).second)
            return {Error::InvalidArgument, "FName fields overlap or exceed the declared layout"};
    }
    return {};
}
Status narrowText(const std::vector<std::byte>& bytes, NarrowEncoding encoding, std::string& result) {
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        if (!value) return {Error::InvalidEvidence, "A pool name contains an embedded terminator"};
        if (value >= 128 && encoding == NarrowEncoding::Ascii)
            return {Error::Unsupported, "Non-ASCII narrow names require an explicit encoding contract"};
    }
    const std::string_view text(bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (auto status = validateUtf8(text); !status) {
        status.code = Error::InvalidEvidence;
        return status;
    }
    result.assign(text);
    return {};
}
}
Status validateNamePoolProfile(const NamePoolProfile& profile) { return checkProfile(profile); }
Status validateNameLayout(const NameLayout& layout) { return checkLayout(layout); }
std::string nameLayoutIdentity(const NameLayout& layout, const NamePoolProfile& profile) {
    const auto field = [](const auto& value) { return value ? std::to_string(*value) : "absent"; };
    return "name-pool-v1;profile:" + profile.identity + ";pointer-bytes:" + std::to_string(sizeof(std::uintptr_t)) +
        ";comparison:" + field(layout.comparison) + ";display:" + field(layout.display) + ";number:" + field(layout.number) +
        ";size:" + std::to_string(layout.size) + ";blocks:" + field(profile.blocks) + ";header:" + field(profile.header) +
        ";string:" + field(profile.string) + ";block-bits:" + std::to_string(profile.blockBits) + ";stride:" + std::to_string(profile.stride) +
        ";maximum-blocks:" + std::to_string(profile.maximumBlocks) + ";maximum-units:" + std::to_string(profile.maximumUnits) +
        ";length-shift:" + std::to_string(profile.lengthShift) +
        ";narrow-encoding:" + (profile.narrowEncoding == NarrowEncoding::Ascii ? "ascii" : "utf8") +
        ";outline-numbers:" + (profile.outlineNumbers ? "true" : "false");
}
std::string nameObservationIdentity(const NameLayout& layout, const NamePoolProfile& profile,
    std::uintptr_t poolAddress, std::uint64_t generation) {
    return nameLayoutIdentity(layout, profile) + ";pool-address:" + std::to_string(poolAddress) +
        ";memory-generation:" + std::to_string(generation);
}
Status readPoolName(MemoryReader& reader, std::uintptr_t pool, std::uint32_t id,
    const NamePoolProfile& profile, ReadBudget& budget, std::string& output) {
    output.clear();
    if (auto status = checkProfile(profile); !status) return status;
    const auto blockIndex = id >> profile.blockBits;
    if (blockIndex >= profile.maximumBlocks)
        return {Error::InvalidEvidence, "Name ID exceeds the declared block table"};
    const std::uint64_t blockBytes = (std::uint64_t{1} << profile.blockBits) * profile.stride;
    const std::uint64_t within = (id & ((std::uint32_t{1} << profile.blockBits) - 1)) * std::uint64_t{profile.stride};
    if (*profile.string > blockBytes - within)
        return {Error::InvalidEvidence, "Name entry header crosses the block boundary"};
    std::uintptr_t slot = 0, block = 0, entry = 0, headerAddress = 0, stringAddress = 0;
    if (auto status = addressOf(pool, *profile.blocks + std::uint64_t{blockIndex} * sizeof(block), slot); !status) return status;
    if (auto status = readExact(reader, slot, std::as_writable_bytes(std::span(&block, 1)), budget); !status) return status;
    if (!block) return {Error::InvalidEvidence, "Name pool block is null"};
    if (auto status = addressOf(block, within, entry); !status) return status;
    if (auto status = addressOf(entry, *profile.header, headerAddress); !status) return status;
    if (auto status = addressOf(entry, *profile.string, stringAddress); !status) return status;
    std::uint16_t header = 0;
    if (auto status = readExact(reader, headerAddress, std::as_writable_bytes(std::span(&header, 1)), budget); !status) return status;
    const auto length = header >> profile.lengthShift;
    const bool wide = header & 1;
    const auto byteCount = static_cast<std::size_t>(length) * (wide ? 2 : 1);
    if (!length || static_cast<std::uint32_t>(length) > profile.maximumUnits || byteCount > blockBytes - within - *profile.string)
        return {Error::InvalidEvidence, "Name length exceeds the entry or block bounds"};
    std::vector<std::byte> bytes(byteCount);
    if (auto status = readExact(reader, stringAddress, bytes, budget); !status) return status;
    std::string text;
    if (wide) {
        std::vector<char16_t> units(static_cast<std::size_t>(length));
        std::memcpy(units.data(), bytes.data(), bytes.size());
        for (auto unit : units) if (!unit) return {Error::InvalidEvidence, "A pool name contains an embedded terminator"};
        if (auto status = decodeUtf16(units, text); !status) return {Error::InvalidEvidence, status.message};
    } else if (auto status = narrowText(bytes, profile.narrowEncoding, text); !status) return status;
    std::uintptr_t finalBlock = 0;
    std::uint16_t finalHeader = 0;
    std::vector<std::byte> finalBytes(byteCount);
    if (auto status = readExact(reader, slot, std::as_writable_bytes(std::span(&finalBlock, 1)), budget); !status) return status;
    if (auto status = readExact(reader, headerAddress, std::as_writable_bytes(std::span(&finalHeader, 1)), budget); !status) return status;
    if (auto status = readExact(reader, stringAddress, finalBytes, budget); !status) return status;
    if (block != finalBlock || header != finalHeader || bytes != finalBytes)
        return {Error::InvalidEvidence, "Name entry changed during observation"};
    output = std::move(text);
    return {};
}
Status readFName(MemoryReader& reader, std::uintptr_t address, const NameLayout& layout,
    std::uintptr_t pool, const NamePoolProfile& profile, ReadBudget& budget, std::string& output) {
    output.clear();
    if (auto status = checkLayout(layout); !status) return status;
    if (auto status = checkProfile(profile); !status) return status;
    std::array<std::byte, 16> bytes{}, final{};
    if (auto status = readExact(reader, address, std::span(bytes).first(layout.size), budget); !status) return status;
    auto field = [&bytes](std::uint32_t offset) { std::uint32_t value; std::memcpy(&value, bytes.data() + offset, sizeof(value)); return value; };
    if ((field(*layout.comparison) >> profile.blockBits) >= profile.maximumBlocks)
        return {Error::InvalidEvidence, "FName comparison ID exceeds the declared block table"};
    const auto number = layout.number ? field(*layout.number) : 0;
    if (number > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
        return {Error::InvalidEvidence, "FName number exceeds the supported nonnegative int32 contract"};
    const auto id = field(layout.display ? *layout.display : *layout.comparison);
    std::string entry;
    if (auto status = readPoolName(reader, pool, id, profile, budget, entry); !status) return status;
    auto name = fnameToString(entry, number);
    if (auto status = readExact(reader, address, std::span(final).first(layout.size), budget); !status) return status;
    if (bytes != final) return {Error::InvalidEvidence, "FName fields changed during observation"};
    output = std::move(name);
    return {};
}
Status probeNameField(MemoryReader& reader, std::span<const NameSample> samples, std::uint32_t extent,
    const NameLayout& layout, std::uintptr_t pool, const NamePoolProfile& profile, ReadBudget& budget,
    FieldProbeReport& report) {
    report = {}; report.generation = budget.generation;
    if (auto status = checkLayout(layout); !status) return status;
    if (auto status = checkProfile(profile); !status) return status;
    if (samples.empty() || samples.size() > 64 || extent < layout.size || extent > 4096)
        return {Error::InvalidArgument, "Name probing requires samples and a bounded object extent"};
    std::set<std::uintptr_t> unique;
    for (const auto& sample : samples)
        if (sample.expected.empty() || sample.expected.size() > profile.maximumUnits * 4 || sample.identity.empty() || !unique.insert(sample.object).second)
            return {Error::InvalidArgument, "Name samples require distinct objects, bounded names and identities"};
    std::vector<Offset> observed;
    for (std::uint32_t offset = 0; offset <= extent - layout.size; offset += 4) {
        ++report.examinedOffsets;
        bool matched = true;
        for (const auto& sample : samples) {
            std::uintptr_t address = 0;
            if (auto status = addressOf(sample.object, offset, address); !status) return status;
            std::string name;
            const auto status = readFName(reader, address, layout, pool, profile, budget, name);
            if (!status && status.code != Error::InvalidEvidence && status.code != Error::Unmapped &&
                status.code != Error::Overflow && status.code != Error::Unsupported) return status;
            if (!status || name != sample.expected) {
                report.rejected.push_back({offset, status ? Error::InvalidEvidence : status.code, sample.identity,
                    status ? "Name differs from the declared anchor" : status.message});
                matched = false; break;
            }
        }
        if (matched) { Offset value; value.value = offset; observed.push_back(std::move(value)); }
    }
    for (auto& offset : observed) {
        Evidence evidence{"exact FName matches across distinct supplied anchors and a final recheck", true, samples.size(), {*offset.value}};
        evidence.source = nameObservationIdentity(layout, profile, pool, budget.generation);
        for (const auto& sample : samples) {
            std::string name;
            std::uintptr_t address = 0;
            if (auto status = addressOf(sample.object, *offset.value, address); !status) return status;
            if (auto status = readFName(reader, address, layout, pool, profile, budget, name); !status) return status;
            if (name != sample.expected) return {Error::InvalidEvidence, "FName anchor changed during field probing"};
            evidence.sampleIdentities.push_back(sample.identity + ";expected-name:" + sample.expected);
        }
        offset.evidence.push_back(std::move(evidence));
        if (observed.size() == 1 && samples.size() >= 2) offset.validation = Validation::Validated;
    }
    report.candidates = std::move(observed);
    return {};
}
}
