#include "andueprober/Discovery.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>

namespace andueprober {
namespace {
constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
Status invalid(const char* text) { return {Error::InvalidEvidence, text}; }
Status add(std::uintptr_t base, std::uint64_t offset, std::uintptr_t& out) {
    if (offset > maximum - base) return {Error::Overflow, "Module address overflow"};
    out = base + static_cast<std::uintptr_t>(offset); return {};
}
Status relative(std::uintptr_t base, std::int64_t offset, std::uintptr_t& out) {
    if (offset >= 0) return add(base, static_cast<std::uint64_t>(offset), out);
    const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
    if (magnitude > base) return {Error::Overflow, "Module address underflow"};
    out = base - magnitude; return {};
}
std::uint64_t number(const std::byte* bytes, std::size_t size) {
    std::uint64_t result = 0;
    for (std::size_t i = 0; i < size; ++i) result |= std::uint64_t(std::to_integer<unsigned>(bytes[i])) << (i * 8);
    return result;
}
const ModuleRange* contains(const ModuleImage& module, std::uintptr_t address, std::size_t size, bool executable = false) {
    for (const auto& range : module.ranges)
        if (range.readable && (!executable || range.executable) && address >= range.start &&
            address - range.start <= range.size && size <= range.size - (address - range.start)) return &range;
    return nullptr;
}
Status validate(MemoryReader& reader, const ModuleImage& module, ReadBudget& budget) {
    if (sizeof(std::uintptr_t) != 8) return {Error::Unsupported, "AArch64 discovery requires a 64-bit address provider"};
    if (module.identity.empty() || module.ranges.empty() || module.ranges.size() > 64)
        return {Error::InvalidArgument, "A bounded module identity and load ranges are required"};
    if (!module.generation || module.generation != reader.generation() || module.generation != budget.generation)
        return {Error::StaleIdentity, "Discovery module generation does not match the read lease"};
    for (std::size_t i = 0; i < module.ranges.size(); ++i) {
        const auto& range = module.ranges[i];
        if (range.start < module.loadBias || !range.size || range.size > maximum - range.start) return {Error::Overflow, "Invalid module load range"};
        for (std::size_t j = 0; j < i; ++j)
            if (range.start < module.ranges[j].start + module.ranges[j].size &&
                module.ranges[j].start < range.start + range.size) return invalid("Overlapping module load ranges");
    }
    return readExact(reader, module.ranges.front().start, {}, budget);
}
Status word(MemoryReader& reader, const ModuleImage& module, std::uintptr_t address, ReadBudget& budget, std::uint32_t& value) {
    if ((address & 3) || !contains(module, address, 4, true)) return {Error::Unmapped, "Instruction is outside readable executable module ranges"};
    std::array<std::byte, 4> bytes;
    auto status = readExact(reader, address, bytes, budget);
    if (status) value = static_cast<std::uint32_t>(number(bytes.data(), 4));
    return status;
}
Status page(std::uint32_t instruction, std::uintptr_t pc, std::uintptr_t& result) {
    std::int64_t immediate = ((instruction >> 5) & 0x7ffffu) * 4u + ((instruction >> 29) & 3u);
    if (immediate & (1 << 20)) immediate -= 1 << 21;
    // ADRP always uses an architectural 4096-byte page, including on 16 KiB Android.
    return relative(pc & ~std::uintptr_t{4095}, immediate * 4096, result);
}
Status branch(std::uint32_t instruction, std::uintptr_t pc, std::uintptr_t& result) {
    std::int64_t immediate = instruction & 0x03ffffffu;
    if (immediate & (1 << 25)) immediate -= 1 << 26;
    return relative(pc, immediate * 4, result);
}
Status unique(std::optional<std::uintptr_t>& found, std::uintptr_t value) {
    if (found && *found != value) return invalid("Discovery has multiple candidates");
    found = value; return {};
}
void publish(const ModuleImage& module, std::uintptr_t address, const char* check,
    std::vector<std::uintptr_t> observations, DiscoveryValue& out) {
    out.address = address;
    out.evidence = {check, true, 1, {}, module.identity, {"lease:" + std::to_string(module.generation)}};
    for (auto observation : observations) out.evidence.relativeAddresses.push_back(observation - module.loadBias);
}
template<class Visit>
Status scan(MemoryReader& reader, const ModuleImage& module, bool executable, std::size_t width,
    std::size_t alignment, ReadBudget& budget, Visit visit) {
    std::array<std::byte, 4096 + 512> bytes;
    for (const auto& range : module.ranges) {
        if (!range.readable || (executable && !range.executable) || range.size < width) continue;
        std::size_t offset = executable ? ((4 - (range.start & 3)) & 3) : 0;
        while (offset <= range.size - width) {
            const auto primary = std::min<std::size_t>(4096, range.size - offset - width + 1);
            const auto count = primary + width - 1;
            auto status = readExact(reader, range.start + offset, std::span(bytes).first(count), budget);
            if (!status) return status;
            for (std::size_t i = 0; i < primary; i += alignment) {
                status = visit(range.start + offset + i, bytes.data() + i);
                if (!status) return status;
            }
            offset += executable ? ((primary + 3) & ~std::size_t{3}) : primary;
        }
    }
    return {};
}
Status literal(MemoryReader& reader, const ModuleImage& module, const char16_t* text,
    std::size_t units, ReadBudget& budget, std::uintptr_t& address) {
    std::vector<std::byte> pattern(units * 2);
    for (std::size_t i = 0; i < units; ++i) {
        pattern[i * 2] = std::byte(text[i] & 255); pattern[i * 2 + 1] = std::byte(text[i] >> 8);
    }
    std::optional<std::uintptr_t> found;
    auto status = scan(reader, module, false, pattern.size(), 1, budget, [&](auto pc, const auto* bytes) {
        return std::memcmp(bytes, pattern.data(), pattern.size()) == 0 ? unique(found, pc) : Status{};
    });
    if (!status) return status;
    if (!found) return invalid("Discovery string was not found in the selected module");
    address = *found; return {};
}
Status prologue(MemoryReader& reader, const ModuleImage& module, std::uintptr_t reference,
    bool nameFunction, ReadBudget& budget, std::uintptr_t& start) {
    const auto* range = contains(module, reference, 4, true);
    if (!range) return {Error::Unmapped, "Reference is outside the executable module"};
    const auto limit = nameFunction ? 512u : 256u;
    for (unsigned i = 0; i < limit && std::uintptr_t(i) * 4 <= reference - range->start; ++i) {
        const auto pc = reference - i * 4;
        std::uint32_t instruction;
        auto status = word(reader, module, pc, budget, instruction); if (!status) return status;
        if (nameFunction ? (instruction & 0xff8003ffu) == 0xd10003ffu :
            (instruction & 0xfc407fffu) == 0xa8007bfdu) { start = pc; return {}; }
    }
    return invalid("A supported function prologue was not found within the bounded window");
}
Status callTarget(MemoryReader& reader, const ModuleImage& module, std::uintptr_t start,
    bool nameFunction, ReadBudget& budget, std::uintptr_t& target) {
    const auto* range = contains(module, start, 4, true);
    if (!range) return {Error::Unmapped, "Function is outside the executable module"};
    std::optional<std::uintptr_t> candidate;
    bool terminated = false;
    const auto size = std::min<std::size_t>(nameFunction ? 256 : 512, range->size - (start - range->start));
    for (std::size_t offset = 0; offset + 4 <= size; offset += 4) {
        std::uint32_t instruction;
        auto status = word(reader, module, start + offset, budget, instruction); if (!status) return status;
        if (instruction == 0xd65f03c0u) { terminated = true; break; }
        if ((instruction & 0xfc000000u) == 0x94000000u) {
            std::uintptr_t destination;
            status = branch(instruction, start + offset, destination); if (!status) return status;
            if (!contains(module, destination, 4, true)) return {Error::Unmapped, "Call target is outside the executable module"};
            if (nameFunction) { status = unique(candidate, destination); if (!status) return status; }
            else candidate = destination;
        } else if ((instruction & 0xfc000000u) == 0x14000000u) {
            if (nameFunction)
                return {Error::Unsupported, "Name-function discovery does not infer control flow through unconditional branches"};
            std::uintptr_t destination;
            status = branch(instruction, start + offset, destination); if (!status) return status;
            if (!contains(module, destination, 4, true))
                return {Error::Unmapped, "Tail-call target is outside the executable module"};
            candidate = destination;
            terminated = true;
            break;
        }
    }
    if (!terminated || !candidate) return invalid("Discovery requires a bounded return and a call candidate");
    target = *candidate; return {};
}
}
Status readModuleImage(MemoryReader& reader, std::uintptr_t elfAddress, std::uintptr_t bias,
    std::string identity, ReadBudget& budget, ModuleImage& out) {
    out = {};
    if (sizeof(std::uintptr_t) != 8) return {Error::Unsupported, "ELF64 discovery requires a 64-bit address provider"};
    if (identity.empty() || !budget.generation) return {Error::InvalidArgument, "ELF discovery requires a leased module identity"};
    std::array<std::byte, 64> header;
    auto status = readExact(reader, elfAddress, header, budget); if (!status) return status;
    const auto* h = header.data();
    if (number(h, 4) != 0x464c457f || number(h + 4, 3) != 0x010102 || number(h + 16, 2) != 3 ||
        number(h + 18, 2) != 183 || number(h + 20, 4) != 1 || number(h + 52, 2) != 64 || number(h + 54, 2) != 56)
        return {Error::Unsupported, "Discovery requires an ELF64 little-endian AArch64 shared object"};
    const auto count = number(h + 56, 2), tableOffset = number(h + 32, 8);
    if (!count || count > 64 || tableOffset < 64 || tableOffset > 1024 * 1024) return invalid("ELF program-header bounds are invalid");
    std::uintptr_t tableAddress;
    status = add(elfAddress, tableOffset, tableAddress); if (!status) return status;
    std::vector<std::byte> table(count * 56);
    status = readExact(reader, tableAddress, table, budget); if (!status) return status;
    ModuleImage image{std::move(identity), budget.generation, bias, {}};
    for (std::size_t i = 0; i < count; ++i) {
        const auto* p = table.data() + i * 56;
        if (number(p, 4) != 1) continue;
        const auto size = number(p + 40, 8), fileSize = number(p + 32, 8);
        const auto alignment = number(p + 48, 8), virtualAddress = number(p + 16, 8), fileOffset = number(p + 8, 8);
        if ((alignment > 1 && ((alignment & (alignment - 1)) || virtualAddress % alignment != fileOffset % alignment)) ||
            fileOffset > std::numeric_limits<std::uint64_t>::max() - fileSize || (number(p + 4, 4) & ~7u))
            return invalid("ELF segment alignment or file bounds are invalid");
        if (fileSize > size || size > std::numeric_limits<std::size_t>::max()) return invalid("ELF segment size is invalid");
        if (!size) continue;
        std::uintptr_t address;
        status = add(bias, number(p + 16, 8), address); if (!status) return status;
        image.ranges.push_back({address, static_cast<std::size_t>(size), bool(number(p + 4, 4) & 4), bool(number(p + 4, 4) & 1)});
    }
    status = validate(reader, image, budget); if (!status) return status;
    if (!contains(image, elfAddress, header.size()) || !contains(image, tableAddress, table.size()))
        return invalid("ELF headers must reside in readable load ranges");
    std::array<std::byte, 64> check;
    std::vector<std::byte> checkTable(table.size());
    status = readExact(reader, elfAddress, check, budget); if (!status) return status;
    status = readExact(reader, tableAddress, checkTable, budget); if (!status) return status;
    if (check != header || checkTable != table) return {Error::StaleIdentity, "ELF headers changed during discovery"};
    out = std::move(image); return {};
}
Status readModuleImageFromFile(MemoryReader& reader, const std::string& modulePath,
    std::uintptr_t bias, std::string identity, ReadBudget& budget, ModuleImage& out) {
    out = {};
    if (sizeof(std::uintptr_t) != 8) return {Error::Unsupported, "ELF64 discovery requires a 64-bit address provider"};
    if (modulePath.empty() || modulePath.size() >= 4096 || modulePath.find('\0') != std::string::npos ||
        identity.empty() || !budget.generation)
        return {Error::InvalidArgument, "File-backed ELF discovery requires bounded leased metadata"};
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "File-backed ELF discovery cancelled"};
    if (std::chrono::steady_clock::now() >= budget.deadline)
        return {Error::DeadlineExceeded, "File-backed ELF discovery deadline expired"};
    std::ifstream input(modulePath, std::ios::binary);
    if (!input) return {Error::PermissionDenied, "The mapped module file is unreadable"};
    std::array<std::byte, 64> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    if (input.gcount() != static_cast<std::streamsize>(header.size())) return {Error::ShortRead, "The mapped ELF header is truncated"};
    const auto* h = header.data();
    if (number(h, 4) != 0x464c457f || number(h + 4, 3) != 0x010102 || number(h + 16, 2) != 3 ||
        number(h + 18, 2) != 183 || number(h + 20, 4) != 1 || number(h + 52, 2) != 64 || number(h + 54, 2) != 56)
        return {Error::Unsupported, "Discovery requires an ELF64 little-endian AArch64 shared object"};
    const auto count = number(h + 56, 2), tableOffset = number(h + 32, 8);
    if (!count || count > 64 || tableOffset < 64 || tableOffset > 1024 * 1024)
        return invalid("ELF program-header bounds are invalid");
    std::vector<std::byte> table(count * 56);
    input.seekg(static_cast<std::streamoff>(tableOffset));
    input.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()));
    if (input.gcount() != static_cast<std::streamsize>(table.size())) return {Error::ShortRead, "The mapped ELF program headers are truncated"};
    ModuleImage image{std::move(identity), budget.generation, bias, {}};
    for (std::size_t index = 0; index < count; ++index) {
        const auto* program = table.data() + index * 56;
        if (number(program, 4) != 1) continue;
        const auto size = number(program + 40, 8), fileSize = number(program + 32, 8);
        const auto alignment = number(program + 48, 8), virtualAddress = number(program + 16, 8);
        const auto fileOffset = number(program + 8, 8), flags = number(program + 4, 4);
        if ((alignment > 1 && ((alignment & (alignment - 1)) || virtualAddress % alignment != fileOffset % alignment)) ||
            fileOffset > std::numeric_limits<std::uint64_t>::max() - fileSize || (flags & ~7u) || fileSize > size ||
            size > std::numeric_limits<std::size_t>::max()) return invalid("ELF segment alignment or file bounds are invalid");
        if (!size) continue;
        std::uintptr_t address = 0;
        if (auto status = add(bias, virtualAddress, address); !status) return status;
        image.ranges.push_back({address, static_cast<std::size_t>(size), bool(flags & 4), bool(flags & 1)});
    }
    if (auto status = validate(reader, image, budget); !status) return status;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "File-backed ELF discovery cancelled"};
    if (std::chrono::steady_clock::now() >= budget.deadline)
        return {Error::DeadlineExceeded, "File-backed ELF discovery deadline expired"};
    std::ifstream check(modulePath, std::ios::binary);
    std::array<std::byte, 64> headerCheck{};
    check.read(reinterpret_cast<char*>(headerCheck.data()), headerCheck.size());
    check.seekg(static_cast<std::streamoff>(tableOffset));
    std::vector<std::byte> tableCheck(table.size());
    check.read(reinterpret_cast<char*>(tableCheck.data()), static_cast<std::streamsize>(tableCheck.size()));
    if (!check || headerCheck != header || tableCheck != table)
        return {Error::StaleIdentity, "Mapped ELF file metadata changed during discovery"};
    out = std::move(image);
    return {};
}
Status findAdrpReference(MemoryReader& reader, const ModuleImage& module, std::uintptr_t target,
    ReadBudget& budget, DiscoveryValue& out) {
    out = {};
    auto status = validate(reader, module, budget); if (!status) return status;
    if (!contains(module, target, 1)) return {Error::Unmapped, "Reference target is outside the readable module"};
    std::optional<std::uintptr_t> found;
    status = scan(reader, module, true, 8, 4, budget, [&](auto pc, const auto* bytes) {
        const auto adrp = static_cast<std::uint32_t>(number(bytes, 4));
        const auto instruction = static_cast<std::uint32_t>(number(bytes + 4, 4));
        if ((adrp & 0x9f000000u) != 0x90000000u || (instruction & 0xff800000u) != 0x91000000u ||
            ((instruction >> 5) & 31u) != (adrp & 31u) || (adrp & 31u) == 31) return Status{};
        std::uintptr_t address;
        auto result = page(adrp, pc, address); if (!result) return result;
        result = add(address, std::uint64_t((instruction >> 10) & 4095u) << ((instruction & (1u << 22)) ? 12 : 0), address);
        if (!result) return result;
        return address == target ? unique(found, pc) : Status{};
    });
    if (!status) return status;
    if (!found) return invalid("A unique adjacent ADRP and ADD reference was not found");
    publish(module, *found, "aarch64-adjacent-adrp-add-candidate", {*found, target}, out); return {};
}
Status findAddressFromAarch64Pattern(MemoryReader& reader, const ModuleImage& module,
    std::span<const std::byte> pattern, std::span<const std::byte> mask,
    std::int32_t instructionOffset, ReadBudget& budget, DiscoveryValue& out) {
    out = {};
    auto status = validate(reader, module, budget); if (!status) return status;
    if (pattern.size() < 4 || pattern.size() > 256 || pattern.size() != mask.size() ||
        instructionOffset < -1024 || instructionOffset > 1024)
        return {Error::InvalidArgument, "A bounded AArch64 byte pattern and mask are required"};
    for (const auto value : mask)
        if (value != std::byte{0} && value != std::byte{0xff})
            return {Error::InvalidArgument, "AArch64 pattern masks support only wildcard and exact bytes"};
    std::optional<std::uintptr_t> found;
    std::uintptr_t observedMatch = 0, observedInstruction = 0;
    status = scan(reader, module, true, pattern.size(), 1, budget, [&](auto match, const auto* bytes) {
        for (std::size_t index = 0; index < pattern.size(); ++index)
            if (mask[index] == std::byte{0xff} && bytes[index] != pattern[index]) return Status{};
        std::uintptr_t instructionAddress;
        auto result = relative(match, instructionOffset, instructionAddress); if (!result) return result;
        if (instructionAddress & 3u) return Status{};
        std::uint32_t adrp;
        result = word(reader, module, instructionAddress, budget, adrp); if (!result) return result;
        if ((adrp & 0x9f000000u) != 0x90000000u || (adrp & 31u) == 31)
            return invalid("The profile pattern does not identify an ADRP instruction");
        std::uintptr_t base;
        result = page(adrp, instructionAddress, base); if (!result) return result;
        std::optional<std::uintptr_t> decoded;
        for (std::size_t step = 1; step < 8; ++step) {
            std::uint32_t next;
            result = word(reader, module, instructionAddress + step * 4, budget, next); if (!result) return result;
            const auto source = (next >> 5) & 31u;
            if (source != (adrp & 31u)) continue;
            std::uintptr_t address = base;
            if ((next & 0xff800000u) == 0x91000000u) {
                result = add(address, std::uint64_t((next >> 10) & 4095u) << ((next & (1u << 22)) ? 12 : 0), address);
            } else if ((next & 0xffc00000u) == 0xf9400000u) {
                result = add(address, std::uint64_t((next >> 10) & 4095u) * 8, address);
            } else continue;
            if (!result) return result;
            if (!contains(module, address, 1)) continue;
            result = unique(decoded, address); if (!result) return result;
        }
        if (!decoded) return invalid("The profile ADRP has no bounded ADD/LDR address consumer");
        result = unique(found, *decoded); if (!result) return result;
        observedMatch = match;
        observedInstruction = instructionAddress;
        return Status{};
    });
    if (!status) return status;
    if (!found) return invalid("The AArch64 profile pattern was not found");
    publish(module, *found, "aarch64-masked-profile-address-candidate",
        {observedMatch, observedInstruction, *found}, out);
    return {};
}
Status findObjectArrayCandidate(MemoryReader& reader, const ModuleImage& module, ReadBudget& budget, DiscoveryValue& out) {
    out = {};
    auto status = validate(reader, module, budget); if (!status) return status;
    constexpr char16_t text[] = u"Game engine shut down";
    std::uintptr_t stringAddress, start, target;
    status = literal(reader, module, text, std::size(text), budget, stringAddress); if (!status) return status;
    DiscoveryValue reference;
    status = findAdrpReference(reader, module, stringAddress, budget, reference); if (!status) return status;
    status = prologue(reader, module, *reference.address, false, budget, start); if (!status) return status;
    status = callTarget(reader, module, start, false, budget, target); if (!status) return status;
    const auto* range = contains(module, target, 4, true);
    const auto size = std::min<std::size_t>(128, range->size - (target - range->start));
    std::optional<std::uintptr_t> found;
    std::uintptr_t observedSlot = 0;
    std::size_t adrpCount = 0, consumerCount = 0, moduleSlotCount = 0, modulePointerCount = 0;
    for (std::size_t offset = 0; offset + 8 <= size; offset += 4) {
        std::uint32_t adrp;
        status = word(reader, module, target + offset, budget, adrp); if (!status) return status;
        if (adrp == 0xd65f03c0u) break;
        if ((adrp & 0x9f000000u) != 0x90000000u || (adrp & 31u) == 31) continue;
        ++adrpCount;
        for (std::size_t distance = 4; distance < 32 && offset + distance + 4 <= size; distance += 4) {
            std::uint32_t ldr;
            status = word(reader, module, target + offset + distance, budget, ldr); if (!status) return status;
            if ((ldr & 0xffc00000u) != 0xf9400000u || ((ldr >> 5) & 31u) != (adrp & 31u)) continue;
            ++consumerCount;
            std::uintptr_t slot;
            status = page(adrp, target + offset, slot); if (!status) return status;
            status = add(slot, std::uint64_t((ldr >> 10) & 4095u) * 8, slot); if (!status) return status;
            if (!contains(module, slot, 8)) continue;
            ++moduleSlotCount;
            std::array<std::byte, 8> bytes, check;
            status = readExact(reader, slot, bytes, budget); if (!status) return status;
            const auto address = number(bytes.data(), 8);
            if (!contains(module, address, 8)) continue;
            ++modulePointerCount;
            status = readExact(reader, slot, check, budget); if (!status) return status;
            if (check != bytes) return {Error::StaleIdentity, "Global pointer slot changed during discovery"};
            status = unique(found, address); if (!status) return status;
            observedSlot = slot;
        }
    }
    if (!found) return {Error::InvalidEvidence,
        "A unique bounded ADRP and LDR object-array candidate was not found at relative tail target " +
        std::to_string(target - module.loadBias) + " (ADRP=" + std::to_string(adrpCount) +
        ", consumers=" + std::to_string(consumerCount) + ", module-slots=" +
        std::to_string(moduleSlotCount) + ", module-pointers=" + std::to_string(modulePointerCount) + ")"};
    publish(module, *found, "aarch64-finish-destroy-object-array-candidate", {stringAddress, *reference.address, start, target, observedSlot, *found}, out);
    return {};
}
Status findNamePoolCandidate(MemoryReader& reader, const ModuleImage& module, ReadBudget& budget, DiscoveryValue& out) {
    out = {};
    auto status = validate(reader, module, budget); if (!status) return status;
    struct Pattern {
        std::vector<std::byte> bytes;
        std::vector<std::byte> mask;
        std::int32_t instructionOffset = 0;
    };
    const auto parse = [](std::string_view text, std::int32_t instructionOffset) {
        Pattern result{{}, {}, instructionOffset};
        const auto hex = [](char value) -> unsigned {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            return value - 'A' + 10;
        };
        for (std::size_t position = 0; position < text.size();) {
            while (position < text.size() && text[position] == ' ') ++position;
            if (position == text.size()) break;
            const auto end = text.find(' ', position);
            const auto token = text.substr(position, end == std::string_view::npos ? text.size() - position : end - position);
            if (token == "?") {
                result.bytes.push_back(std::byte{0});
                result.mask.push_back(std::byte{0});
            } else {
                result.bytes.push_back(std::byte{static_cast<unsigned char>((hex(token[0]) << 4) | hex(token[1]))});
                result.mask.push_back(std::byte{0xff});
            }
            position = end == std::string_view::npos ? text.size() : end + 1;
        }
        return result;
    };
    const std::array patterns{
        parse("F4 4F 01 A9 FD 7B 02 A9 FD 83 00 91 ? ? ? ? ? ? ? ? A8 02 ? 39", 0x18),
        parse("F4 4F 01 A9 FD 7B 02 A9 FD 83 00 91 ? ? ? ? A8 02 ? 39", 0x24),
        parse("FD 7B 01 A9 FD 43 00 91 ? ? ? ? 89 ? ? 39 F3 03 08 AA C9 00 00 37 ? ? ? ? ? ? ? 91", 0x18),
        parse("F8 C8 ? ? 39 C8 00 00 37 ? ? ? ? ? ? ? 91", 9),
        parse("02 ? 91 C8 00 00 37 ? ? ? ? ? ? ? 91", 7),
        parse("39 C8 00 00 37 ? ? ? ? ? ? ? 91 ? ? ? 97 ? 00 80 52 ? ? ? 39", 5),
        parse("C8 00 00 37 ? ? ? ? ? ? ? 91 ? ? ? 97 ? 00 80 52", 4),
        parse("C8 00 00 37 ? ? ? ? ? ? ? 91 ? ? ? 97", 4),
    };
    std::size_t width = 0;
    for (const auto& pattern : patterns) width = std::max(width, pattern.bytes.size());
    std::optional<std::uintptr_t> found;
    std::uintptr_t observedMatch = 0, observedInstruction = 0;
    status = scan(reader, module, true, width, 1, budget, [&](auto match, const auto* bytes) {
        for (const auto& pattern : patterns) {
            bool matches = true;
            for (std::size_t index = 0; index < pattern.bytes.size(); ++index)
                if (pattern.mask[index] == std::byte{0xff} && bytes[index] != pattern.bytes[index]) {
                    matches = false;
                    break;
                }
            if (!matches) continue;
            std::uintptr_t instructionAddress = 0;
            auto decodedStatus = relative(match, pattern.instructionOffset, instructionAddress);
            if (!decodedStatus) return decodedStatus;
            if ((instructionAddress & 3u) || !contains(module, instructionAddress, 32, true)) continue;
            std::uint32_t adrp = 0;
            decodedStatus = word(reader, module, instructionAddress, budget, adrp);
            if (!decodedStatus) return decodedStatus;
            if ((adrp & 0x9f000000u) != 0x90000000u || (adrp & 31u) == 31) continue;
            std::uintptr_t pageAddress = 0;
            decodedStatus = page(adrp, instructionAddress, pageAddress);
            if (!decodedStatus) return decodedStatus;
            std::optional<std::uintptr_t> decoded;
            for (std::size_t step = 1; step < 8; ++step) {
                std::uint32_t next = 0;
                decodedStatus = word(reader, module, instructionAddress + step * 4, budget, next);
                if (!decodedStatus) return decodedStatus;
                if (((next >> 5) & 31u) != (adrp & 31u)) continue;
                auto address = pageAddress;
                if ((next & 0xff800000u) == 0x91000000u)
                    decodedStatus = add(address, std::uint64_t((next >> 10) & 4095u) << ((next & (1u << 22)) ? 12 : 0), address);
                else if ((next & 0xffc00000u) == 0xf9400000u)
                    decodedStatus = add(address, std::uint64_t((next >> 10) & 4095u) * 8, address);
                else continue;
                if (!decodedStatus) return decodedStatus;
                if (!contains(module, address, 1)) continue;
                decodedStatus = unique(decoded, address);
                if (!decodedStatus) return decodedStatus;
            }
            if (!decoded) continue;
            decodedStatus = unique(found, *decoded);
            if (!decodedStatus) return decodedStatus;
            observedMatch = match;
            observedInstruction = instructionAddress;
        }
        return Status{};
    });
    if (!status) return status;
    if (!found) return invalid("The bounded UE NamePoolData instruction forms were not found");
    publish(module, *found, "aarch64-shared-name-pool-address-candidate",
        {observedMatch, observedInstruction, *found}, out);
    return {};
}
Status findNameToStringCandidate(MemoryReader& reader, const ModuleImage& module, ReadBudget& budget, DiscoveryValue& out) {
    out = {};
    auto status = validate(reader, module, budget); if (!status) return status;
    constexpr char16_t text[] = u"%s__Direction_%s";
    std::uintptr_t stringAddress, start, target;
    status = literal(reader, module, text, std::size(text), budget, stringAddress); if (!status) return status;
    DiscoveryValue reference;
    status = findAdrpReference(reader, module, stringAddress, budget, reference); if (!status) return status;
    status = prologue(reader, module, *reference.address, true, budget, start); if (!status) return status;
    status = callTarget(reader, module, start, true, budget, target); if (!status) return status;
    publish(module, target, "aarch64-name-to-string-call-candidate-not-a-signature", {stringAddress, *reference.address, start, target}, out);
    return {};
}
}
