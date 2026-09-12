#include "andueprober/ProcessMemory.hpp"
#include "AndSwapChainHook/Memory.h"
#include <atomic>
#include <algorithm>
#include <array>
#include <filesystem>
#include <cstring>
#include <limits>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <set>
#if defined(__linux__)
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace andueprober {
namespace {
Error convert(AndSwapChainHook::Memory::Error error) {
    using Source = AndSwapChainHook::Memory::Error;
    switch (error) {
    case Source::None: return Error::None;
    case Source::InvalidArgument: return Error::InvalidArgument;
    case Source::AddressOverflow: return Error::Overflow;
    case Source::Unmapped: return Error::Unmapped;
    case Source::PermissionDenied: return Error::PermissionDenied;
    case Source::ShortRead: return Error::ShortRead;
    case Source::Cancelled: return Error::Cancelled;
    case Source::DeadlineExceeded: return Error::DeadlineExceeded;
    case Source::BudgetExceeded: return Error::BudgetExceeded;
    case Source::IdentityChanged: return Error::StaleIdentity;
    case Source::UnsupportedPlatform: return Error::Unsupported;
    case Source::SystemError: return Error::Io;
    }
    return Error::Internal;
}
std::atomic<std::uint64_t> nextLease{1};
#if defined(__linux__)
Status processStartTime(pid_t pid, std::uint64_t& value) {
    value = 0;
    std::ifstream input("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!input || !std::getline(input, line) || line.size() > 4096)
        return {Error::Io, "Cannot read the target process identity"};
    const auto end = line.rfind(')');
    if (end == std::string::npos || end + 2 >= line.size())
        return {Error::InvalidEvidence, "The target process identity is malformed"};
    std::istringstream fields(line.substr(end + 2));
    std::string field;
    for (int number = 3; number <= 22; ++number) {
        if (!(fields >> field)) return {Error::InvalidEvidence, "The target process identity is truncated"};
        if (number == 22) {
            try { value = std::stoull(field); }
            catch (...) { return {Error::InvalidEvidence, "The target process generation is invalid"}; }
        }
    }
    return value ? Status{} : Status{Error::InvalidEvidence, "The target process generation is zero"};
}

Status fileBuildId(const std::string& path, std::string& result) {
    result.clear();
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {Error::PermissionDenied, "Cannot open the mapped module file"};
    struct Close { int fd; ~Close() { if (fd >= 0) ::close(fd); } } close{fd};
    Elf64_Ehdr header{};
    if (::pread(fd, &header, sizeof(header), 0) != sizeof(header) ||
        std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum || header.e_phnum > 64)
        return {Error::InvalidEvidence, "The mapped module file has an invalid ELF header"};
    std::vector<Elf64_Phdr> programs(header.e_phnum);
    const auto programBytes = programs.size() * sizeof(Elf64_Phdr);
    if (::pread(fd, programs.data(), programBytes, static_cast<off_t>(header.e_phoff)) !=
        static_cast<ssize_t>(programBytes))
        return {Error::ShortRead, "The mapped module program headers are truncated"};
    for (const auto& program : programs) {
        if (program.p_type != PT_NOTE) continue;
        if (!program.p_filesz || program.p_filesz > 65536)
            return {Error::BudgetExceeded, "The mapped module note exceeds its bound"};
        std::vector<std::byte> bytes(static_cast<std::size_t>(program.p_filesz));
        if (::pread(fd, bytes.data(), bytes.size(), static_cast<off_t>(program.p_offset)) !=
            static_cast<ssize_t>(bytes.size())) return {Error::ShortRead, "The mapped module note is truncated"};
        std::size_t cursor = 0;
        while (cursor < bytes.size()) {
            if (bytes.size() - cursor < sizeof(Elf64_Nhdr))
                return {Error::InvalidEvidence, "The mapped module note header is truncated"};
            Elf64_Nhdr note{};
            std::memcpy(&note, bytes.data() + cursor, sizeof(note));
            cursor += sizeof(note);
            const auto nameBytes = (static_cast<std::uint64_t>(note.n_namesz) + 3) & ~std::uint64_t{3};
            const auto dataBytes = (static_cast<std::uint64_t>(note.n_descsz) + 3) & ~std::uint64_t{3};
            if (nameBytes > bytes.size() - cursor || dataBytes > bytes.size() - cursor - nameBytes)
                return {Error::InvalidEvidence, "The mapped module note exceeds its segment"};
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 && note.n_descsz > 0 && note.n_descsz <= 64 &&
                std::memcmp(bytes.data() + cursor, "GNU", 4) == 0) {
                std::ostringstream hex;
                for (std::size_t index = 0; index < note.n_descsz; ++index)
                    hex << std::hex << std::setw(2) << std::setfill('0') <<
                        std::to_integer<unsigned>(bytes[cursor + nameBytes + index]);
                if (!result.empty() && result != hex.str())
                    return {Error::InvalidEvidence, "The mapped module has conflicting build identifiers"};
                result = hex.str();
            }
            cursor += static_cast<std::size_t>(nameBytes + dataBytes);
        }
    }
    return result.empty() ? Status{Error::Unsupported, "The mapped module has no GNU Build ID"} : Status{};
}

struct RemoteModule {
    std::string path;
    std::uintptr_t bias = 0;
    std::uint64_t inode = 0;
};
Status findRemoteModule(pid_t pid, std::span<const std::string> names, RemoteModule& result) {
    result = {};
    const auto mapsPath = "/proc/" + std::to_string(pid) + "/maps";
    FILE* file = std::fopen(mapsPath.c_str(), "re");
    if (!file) return {Error::PermissionDenied, "Cannot inspect the target module mappings"};
    struct Close { FILE* file; ~Close() { std::fclose(file); } } close{file};
    char* line = nullptr; std::size_t capacity = 0, visited = 0;
    std::set<std::pair<std::string, std::uint64_t>> matches;
    while (getline(&line, &capacity, file) >= 0) {
        if (++visited > 65536) { std::free(line); return {Error::BudgetExceeded, "Target mapping count exceeds its bound"}; }
        unsigned long long begin = 0, end = 0, offset = 0, inode = 0;
        char permissions[5]{}, path[4096]{};
        const auto count = std::sscanf(line, "%llx-%llx %4s %llx %*x:%*x %llu %4095[^\n]",
            &begin, &end, permissions, &offset, &inode, path);
        if (count < 5 || begin >= end) continue;
        if (count != 6) continue;
        std::string candidate(path);
        candidate.erase(0, candidate.find_first_not_of(' '));
        const auto slash = candidate.find_last_of('/');
        const auto basename = slash == std::string::npos ? candidate : candidate.substr(slash + 1);
        if (std::find(names.begin(), names.end(), basename) == names.end()) continue;
        matches.emplace(candidate, inode);
        if (offset == 0) {
            if (result.bias && result.bias != begin) { std::free(line); return {Error::InvalidEvidence, "Target module has multiple load biases"}; }
            result.path = candidate; result.bias = static_cast<std::uintptr_t>(begin); result.inode = inode;
        }
    }
    std::free(line);
    if (std::ferror(file)) return {Error::Io, "Cannot finish reading target mappings"};
    if (matches.size() != 1 || !result.bias || result.path.empty())
        return {Error::InvalidEvidence, "Target module selection is absent or ambiguous"};
    if (*matches.begin() != std::pair{result.path, result.inode})
        return {Error::InvalidEvidence, "Target module mapping identity is inconsistent"};
    return {};
}
#endif
}
struct ProcessMemory::Impl {
    AndSwapChainHook::Memory::ProcessReader reader;
    std::uint64_t epoch = 0;
    std::string moduleIdentity;
    std::uintptr_t elfAddress = 0, loadBias = 0;
    std::string modulePath;
    pid_t targetPid = 0;
    std::uint64_t processStart = 0, moduleInode = 0;
    bool remote = false;
#if defined(__linux__)
    void* lease = nullptr;
    ~Impl() { if (lease) dlclose(lease); }
#endif
};
ProcessMemory::ProcessMemory() : impl_(std::make_unique<Impl>()) {}
ProcessMemory::~ProcessMemory() = default;
Status ProcessMemory::open(const std::string& modulePath, std::uintptr_t address, ReadBudget* sharedBudget) {
    ReadBudget localBudget;
    localBudget.remainingBytes = 256 * 1024;
    auto& budget = sharedBudget ? *sharedBudget : localBudget;
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Module discovery was cancelled"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Module discovery deadline expired"};
    if (impl_->epoch) return {Error::Busy, "Memory provider already owns a module lease"};
#if !defined(__linux__)
    (void)address;
    (void)modulePath;
    return {Error::Unsupported, "The process memory adapter requires Android or Linux"};
#else
    if (!address) return {Error::InvalidArgument, "A live module address is required"};
    if (modulePath.empty() || modulePath.size() >= 4096 || modulePath.find('\0') != std::string::npos)
        return {Error::InvalidArgument, "An explicit bounded module path is required"};
    void* lease = dlopen(modulePath.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (!lease) return {Error::Unsupported, "The linker module cannot provide a NOLOAD lifetime lease"};
    struct PendingLease { void* handle; ~PendingLease() { if (handle) dlclose(handle); } } pending{lease};
    const auto opened = impl_->reader.Open();
    if (!opened) return {convert(opened.error), "Cannot open the public process-memory provider; system error " + std::to_string(opened.systemError)};
    const auto refreshed = impl_->reader.RefreshMappings();
    if (!refreshed) return {convert(refreshed.error), "Cannot refresh module mappings; system error " + std::to_string(refreshed.systemError)};
    struct Notes {
        std::uintptr_t address;
        const std::string& requestedPath;
        ReadBudget& budget;
        struct stat requestedStat{};
        bool requestedStatValid = false;
        std::size_t matchingInstances = 0;
        Error error = Error::None;
        std::array<char, 4096> path{};
        std::array<std::pair<std::uintptr_t, std::size_t>, 64> ranges{};
        std::size_t count = 0;
        bool valid = true, found = false;
        std::uintptr_t bias = 0, elfAddress = 0;
        std::size_t visited = 0;
    } notes{address, modulePath, budget};
    notes.requestedStatValid = ::stat(modulePath.c_str(), &notes.requestedStat) == 0;
    // Bionic holds its linker mutex during this callback. Copy borrowed metadata
    // here and validate the owned path before reading notes outside the callback.
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* opaque) {
        auto& notes = *static_cast<Notes*>(opaque);
        if (notes.budget.cancelled && notes.budget.cancelled->load()) { notes.error = Error::Cancelled; return 1; }
        if (std::chrono::steady_clock::now() >= notes.budget.deadline) { notes.error = Error::DeadlineExceeded; return 1; }
        if (++notes.visited > 4096 || info->dlpi_phnum > 64) { notes.error = Error::BudgetExceeded; return 1; }
        if (info->dlpi_name) {
            const auto length = strnlen(info->dlpi_name, notes.path.size());
            if (length == notes.path.size()) { notes.error = Error::InvalidEvidence; return 1; }
            bool sameFile = notes.requestedPath == info->dlpi_name;
            if (!sameFile && length && notes.requestedStatValid) {
                struct stat candidate{};
                sameFile = ::stat(info->dlpi_name, &candidate) == 0 && candidate.st_dev == notes.requestedStat.st_dev &&
                    candidate.st_ino == notes.requestedStat.st_ino;
            }
            if (sameFile && ++notes.matchingInstances > 1) { notes.error = Error::InvalidEvidence; return 1; }
        }
        const auto base = static_cast<std::uintptr_t>(info->dlpi_addr);
        bool contains = false;
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
            const auto& header = info->dlpi_phdr[i];
            if (header.p_type != PT_LOAD || header.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - base) continue;
            const auto start = base + header.p_vaddr;
            if (notes.address >= start && notes.address - start < header.p_memsz) contains = true;
        }
        if (!contains) return 0;
        notes.found = true;
        notes.bias = base;
        if (!info->dlpi_name) { notes.valid = false; return 1; }
        const auto size = strnlen(info->dlpi_name, notes.path.size());
        if (!size || size == notes.path.size()) { notes.valid = false; return 1; }
        std::memcpy(notes.path.data(), info->dlpi_name, size + 1);
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
            const auto& header = info->dlpi_phdr[i];
            if (header.p_type == PT_LOAD && header.p_offset == 0 && header.p_filesz >= 64 &&
                header.p_vaddr <= std::numeric_limits<std::uintptr_t>::max() - base)
                notes.elfAddress = base + header.p_vaddr;
            if (header.p_type != PT_NOTE) continue;
            if (notes.count == notes.ranges.size() || header.p_memsz > 65536 || header.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - base) {
                notes.valid = false; return 1;
            }
            notes.ranges[notes.count++] = {base + header.p_vaddr, header.p_memsz};
        }
        return 0;
    }, &notes);
    if (notes.error != Error::None) return {notes.error, "The module lease requires one unambiguous normal-linker instance"};
    if (notes.matchingInstances != 1) return {Error::StaleIdentity, "The requested module has no unique loader instance"};
    if (!notes.found) return {Error::StaleIdentity, "The selected module address is no longer loaded"};
    if (!notes.valid || !notes.elfAddress) return {Error::InvalidEvidence, "Module path or note bounds are invalid"};
    std::error_code pathError;
    if (modulePath != notes.path.data() && !std::filesystem::equivalent(modulePath, notes.path.data(), pathError))
        return {Error::StaleIdentity, "The selected address does not belong to the leased module"};
    auto epoch = nextLease.load();
    do {
        if (epoch == std::numeric_limits<std::uint64_t>::max())
            return {Error::Overflow, "Module lease generation capacity exhausted"};
    } while (!nextLease.compare_exchange_weak(epoch, epoch + 1));
    budget.generation = epoch;
    struct OpeningReader final : MemoryReader {
        AndSwapChainHook::Memory::ProcessReader& reader;
        std::uint64_t epoch;
        OpeningReader(AndSwapChainHook::Memory::ProcessReader& r, std::uint64_t e) : reader(r), epoch(e) {}
        std::uint64_t generation() const override { return epoch; }
        ReadResult read(std::uintptr_t address, std::span<std::byte> bytes) override {
            const auto result = reader.Read(address, bytes);
            return {result.bytesRead, convert(result.error), result.systemError};
        }
    } opening(impl_->reader, epoch);
    if (auto status = readExact(opening, address, {}, budget); !status) return status;
    std::string buildId;
    std::size_t noteBudget = 256 * 1024;
    for (const auto& [start, size] : std::span(notes.ranges).first(notes.count)) {
        if (size > noteBudget) return {Error::BudgetExceeded, "Module identity note budget exhausted"};
        noteBudget -= size;
        std::vector<std::byte> bytes(size);
        if (auto status = readExact(opening, start, bytes, budget); !status) return status;
        std::size_t cursor = 0;
        while (cursor < bytes.size()) {
            if (bytes.size() - cursor < sizeof(ElfW(Nhdr)))
                return {Error::InvalidEvidence, "Truncated module note header"};
            ElfW(Nhdr) note;
            std::memcpy(&note, bytes.data() + cursor, sizeof(note));
            cursor += sizeof(note);
            const auto nameSize = (static_cast<std::uint64_t>(note.n_namesz) + 3) & ~std::uint64_t{3};
            const auto descSize = (static_cast<std::uint64_t>(note.n_descsz) + 3) & ~std::uint64_t{3};
            if (nameSize > bytes.size() - cursor || descSize > bytes.size() - cursor - nameSize)
                return {Error::InvalidEvidence, "Module note data exceeds its segment"};
            if (note.n_type == NT_GNU_BUILD_ID && note.n_namesz == 4 &&
                std::memcmp(bytes.data() + cursor, "GNU", 4) == 0 && note.n_descsz > 0 && note.n_descsz <= 64) {
                std::ostringstream hex;
                for (std::size_t i = 0; i < note.n_descsz; ++i)
                    hex << std::hex << std::setw(2) << std::setfill('0') << std::to_integer<unsigned>(bytes[cursor + nameSize + i]);
                if (!buildId.empty() && buildId != hex.str())
                    return {Error::InvalidEvidence, "Module contains conflicting GNU build identifiers"};
                buildId = hex.str();
            }
            cursor += nameSize + descSize;
        }
    }
    if (buildId.empty()) return {Error::Unsupported, "Module requires a bounded GNU build identifier"};
    impl_->moduleIdentity = std::string(notes.path.data()) + "#gnu-build-id:" + buildId;
    impl_->modulePath = notes.path.data();
    impl_->epoch = epoch;
    impl_->loadBias = notes.bias;
    impl_->elfAddress = notes.elfAddress;
    impl_->targetPid = getpid();
    impl_->lease = pending.handle;
    pending.handle = nullptr;
    return {};
#endif
}
Status ProcessMemory::openByName(std::span<const std::string> names, ReadBudget& budget) {
    if (impl_->epoch) return {Error::Busy, "Memory provider already owns a module lease"};
    if (names.empty() || names.size() > 16) return {Error::InvalidArgument, "Module selection requires one to sixteen exact names"};
    for (const auto& name : names)
        if (name.empty() || name.size() > 255 || name.find('/') != std::string::npos)
            return {Error::InvalidArgument, "Module selection requires bounded basenames"};
#if !defined(__linux__)
    return {Error::Unsupported, "Module selection requires the Android or Linux linker"};
#else
    struct Selection {
        std::span<const std::string> names;
        ReadBudget& budget;
        std::array<char, 4096> path{};
        std::uintptr_t address = 0;
        std::size_t visited = 0, matches = 0;
        Error error = Error::None;
    } selection{names, budget};
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* opaque) {
        auto& item = *static_cast<Selection*>(opaque);
        if (item.budget.cancelled && item.budget.cancelled->load()) { item.error = Error::Cancelled; return 1; }
        if (std::chrono::steady_clock::now() >= item.budget.deadline) { item.error = Error::DeadlineExceeded; return 1; }
        if (++item.visited > 4096 || info->dlpi_phnum > 64) { item.error = Error::BudgetExceeded; return 1; }
        if (!info->dlpi_name) return 0;
        const auto length = strnlen(info->dlpi_name, item.path.size());
        if (!length) return 0;
        if (length == item.path.size()) { item.error = Error::InvalidEvidence; return 1; }
        const auto* slash = std::strrchr(info->dlpi_name, '/');
        const auto* basename = slash ? slash + 1 : info->dlpi_name;
        if (std::find(item.names.begin(), item.names.end(), basename) == item.names.end()) return 0;
        if (++item.matches > 1) { item.error = Error::InvalidEvidence; return 1; }
        std::memcpy(item.path.data(), info->dlpi_name, length + 1);
        const auto base = static_cast<std::uintptr_t>(info->dlpi_addr);
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
            const auto& header = info->dlpi_phdr[i];
            if (header.p_type != PT_LOAD || !header.p_memsz) continue;
            if (header.p_vaddr > std::numeric_limits<std::uintptr_t>::max() - base) { item.error = Error::Overflow; return 1; }
            item.address = base + header.p_vaddr; break;
        }
        return 0;
    }, &selection);
    if (selection.error != Error::None) return {selection.error, "Bounded linker module selection failed"};
    if (!selection.matches || !selection.address) return {Error::Unmapped, "No exact normal-linker module name is loaded"};
    return open(selection.path.data(), selection.address, &budget);
#endif
}

Status ProcessMemory::openRemoteByName(pid_t pid, std::span<const std::string> names, ReadBudget& budget) {
    if (impl_->epoch) return {Error::Busy, "Memory provider already owns a module lease"};
    if (pid <= 0 || pid == getpid() || names.empty() || names.size() > 16)
        return {Error::InvalidArgument, "Remote module selection requires another positive PID and exact names"};
    for (const auto& name : names)
        if (name.empty() || name.size() > 255 || name.find('/') != std::string::npos)
            return {Error::InvalidArgument, "Remote module selection requires bounded basenames"};
#if !defined(__linux__)
    (void)budget;
    return {Error::Unsupported, "Remote process memory requires Android or Linux"};
#else
    if (budget.cancelled && budget.cancelled->load()) return {Error::Cancelled, "Remote module selection cancelled"};
    if (std::chrono::steady_clock::now() >= budget.deadline) return {Error::DeadlineExceeded, "Remote module selection deadline expired"};
    RemoteModule module;
    if (auto status = findRemoteModule(pid, names, module); !status) return status;
    struct stat fileStat{};
    if (::stat(module.path.c_str(), &fileStat) != 0 || static_cast<std::uint64_t>(fileStat.st_ino) != module.inode)
        return {Error::StaleIdentity, "The mapped module file identity changed"};
    std::string buildId;
    if (auto status = fileBuildId(module.path, buildId); !status) return status;
    std::uint64_t start = 0;
    if (auto status = processStartTime(pid, start); !status) return status;
    const auto opened = impl_->reader.Open(pid);
    if (!opened) return {convert(opened.error), "Cannot open the explicit target memory channel; system error " + std::to_string(opened.systemError)};
    const auto refreshed = impl_->reader.RefreshMappings();
    if (!refreshed) return {convert(refreshed.error), "Cannot refresh target mappings; system error " + std::to_string(refreshed.systemError)};
    auto epoch = nextLease.load();
    do {
        if (epoch == std::numeric_limits<std::uint64_t>::max())
            return {Error::Overflow, "Module lease generation capacity exhausted"};
    } while (!nextLease.compare_exchange_weak(epoch, epoch + 1));
    impl_->epoch = epoch;
    impl_->moduleIdentity = module.path + "#gnu-build-id:" + buildId + ";pid-start:" + std::to_string(start);
    impl_->modulePath = module.path;
    impl_->elfAddress = module.bias;
    impl_->loadBias = module.bias;
    impl_->targetPid = pid;
    impl_->processStart = start;
    impl_->moduleInode = module.inode;
    impl_->remote = true;
    budget.generation = epoch;
    if (auto status = validateLease(); !status) return status;
    return {};
#endif
}

Status ProcessMemory::validateLease() const {
    if (!impl_->epoch) return {Error::InvalidArgument, "Memory provider has no module lease"};
    if (!impl_->remote) return {};
#if !defined(__linux__)
    return {Error::Unsupported, "Remote process memory requires Android or Linux"};
#else
    std::uint64_t start = 0;
    if (auto status = processStartTime(impl_->targetPid, start); !status) return status;
    if (start != impl_->processStart) return {Error::StaleIdentity, "The target process generation changed"};
    RemoteModule module;
    const std::array<std::string, 2> names{"libUE4.so", "libUnreal.so"};
    if (auto status = findRemoteModule(impl_->targetPid, names, module); !status) return status;
    if (module.path != impl_->modulePath || module.bias != impl_->loadBias || module.inode != impl_->moduleInode)
        return {Error::StaleIdentity, "The target module mapping identity changed"};
    return {};
#endif
}
std::uintptr_t ProcessMemory::elfAddress() const { return impl_->elfAddress; }
std::uintptr_t ProcessMemory::loadBias() const { return impl_->loadBias; }
std::string ProcessMemory::modulePath() const { return impl_->modulePath; }
pid_t ProcessMemory::targetPid() const { return impl_->remote ? impl_->targetPid : getpid(); }
bool ProcessMemory::isRemote() const { return impl_->remote; }
ReadResult ProcessMemory::read(std::uintptr_t address, std::span<std::byte> bytes) {
    if (!impl_->epoch) return {0, Error::InvalidArgument};
    const auto result = impl_->reader.Read(address, bytes);
    return {result.bytesRead, convert(result.error), result.systemError};
}
std::uint64_t ProcessMemory::generation() const { return impl_->epoch; }
std::string ProcessMemory::identity() const { return impl_->moduleIdentity; }
std::string ProcessMemory::providerIdentity() { return ANDUEPROBER_MEMORY_PROVENANCE; }
}
