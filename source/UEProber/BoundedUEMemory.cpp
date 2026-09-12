#include "BoundedUEMemory.hpp"
#include "UE/UEMemory.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace UEMemory {
KittyMemoryMgr kMgr{};
KittyPtrValidator kPtrValidator;
namespace {
thread_local void* readContext = nullptr;
thread_local BoundedRead boundedRead = nullptr;
}
void SetBoundedReader(void* context, BoundedRead reader) noexcept {
    readContext = context;
    boundedRead = reader;
}
void ClearBoundedReader() noexcept {
    boundedRead = nullptr;
    readContext = nullptr;
}
bool vm_rpm_ptr(const void* address, void* result, size_t length) {
    if ((!address && length) || (!result && length) || length > 16 * 1024 * 1024 || !boundedRead)
        return false;
    return boundedRead(readContext, reinterpret_cast<std::uintptr_t>(address), result, length);
}
std::string vm_rpm_str(const void* address, size_t maximum) {
    if (!maximum || maximum > 1024 * 1024) return {};
    std::vector<char> characters(maximum, '\0');
    if (!vm_rpm_ptr(address, characters.data(), characters.size())) return {};
    const auto end = std::find(characters.begin(), characters.end(), '\0');
    return std::string(characters.begin(), end);
}
std::wstring vm_rpm_strw(const void* address, size_t maximum) {
    if (!maximum || maximum > 1024 * 1024 || maximum > std::numeric_limits<size_t>::max() / sizeof(wchar_t)) return {};
    std::vector<wchar_t> characters(maximum, L'\0');
    if (!vm_rpm_ptr(address, characters.data(), characters.size() * sizeof(wchar_t))) return {};
    const auto end = std::find(characters.begin(), characters.end(), L'\0');
    return std::wstring(characters.begin(), end);
}
uintptr_t FindAlignedPointerRefrence(uintptr_t start, size_t range, uintptr_t pointer) {
    if (!start || start != GetPtrAlignedOf(start) || range < sizeof(void*) || range != GetPtrAlignedOf(range)) return 0;
    for (size_t offset = 0; offset <= range - sizeof(void*); offset += sizeof(void*)) {
        uintptr_t value = 0;
        if (!vm_rpm_ptr(reinterpret_cast<void*>(start + offset), &value, sizeof(value))) return 0;
        if (value == pointer) return start + offset;
    }
    return 0;
}
uintptr_t FindAlignedPointerRefrence(uintptr_t remoteBase, const std::vector<char>& buffer, uintptr_t pointer) {
    if (!remoteBase || !pointer || buffer.size() < sizeof(pointer)) return 0;
    for (size_t offset = 0; offset <= buffer.size() - sizeof(pointer); offset += sizeof(void*)) {
        uintptr_t value = 0;
        std::memcpy(&value, buffer.data() + offset, sizeof(value));
        if (value == pointer) return remoteBase + offset;
    }
    return 0;
}
namespace Arm64 {
uintptr_t DecodeADRL(uintptr_t address, uint32_t immediateOffset) {
    if (!address) return 0;
    uint32_t instruction = 0;
    if (!vm_rpm_ptr(reinterpret_cast<void*>(address), &instruction, sizeof(instruction))) return 0;
    const auto adrp = KittyArm64::decodeInsn(instruction, address);
    if (adrp.type != EKittyInsnTypeArm64::ADR && adrp.type != EKittyInsnTypeArm64::ADRP) return 0;
    const auto begin = immediateOffset ? immediateOffset / 4 : 1;
    const auto end = immediateOffset ? begin + 1 : 8;
    for (uint32_t index = begin; index < end; ++index) {
        uint32_t next = 0;
        if (!vm_rpm_ptr(reinterpret_cast<void*>(address + index * 4), &next, sizeof(next))) return 0;
        const auto decoded = KittyArm64::decodeInsn(next);
        if (decoded.isValid() && decoded.immediate != 0 && adrp.rd == decoded.rn)
            return adrp.target + decoded.immediate;
    }
    return 0;
}
}
}

namespace IOUtils {
std::string get_filename(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : path.substr(slash + 1);
}
std::string get_file_directory(const std::string& path) {
    const auto slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}
std::string get_file_extension(const std::string& path) {
    const auto dot = path.find_last_of('.');
    return dot == std::string::npos ? std::string{} : path.substr(dot + 1);
}
bool file_path_contains(const std::string& path, const std::string& part) {
    return !path.empty() && path.find(part) != std::string::npos;
}
std::string remove_specials(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char character) {
        return !std::isalnum(character) && character != '_';
    }), value.end());
    return value;
}
std::string replace_specials(std::string value, char replacement) {
    for (auto& character : value)
        if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_') character = replacement;
    return value;
}
int path_is_directory(const std::string& path) {
    struct stat state{};
    return stat(path.c_str(), &state) == 0 && S_ISDIR(state.st_mode);
}
void delete_directory(const std::string& directory) {
    DIR* handle = opendir(directory.c_str());
    if (!handle) return;
    while (auto* entry = readdir(handle)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        const auto path = directory + '/' + entry->d_name;
        if (path_is_directory(path)) delete_directory(path); else unlink(path.c_str());
    }
    closedir(handle);
    rmdir(directory.c_str());
}
int mkdir_recursive(const std::string& directory, mode_t mode) {
    if (directory.empty() || directory.size() > 4095) { errno = EINVAL; return -1; }
    std::string current = directory;
    for (char* separator = std::strchr(current.data() + 1, '/'); separator; separator = std::strchr(separator + 1, '/')) {
        *separator = '\0';
        if (mkdir(current.c_str(), mode) != 0 && errno != EEXIST) { *separator = '/'; return -1; }
        *separator = '/';
    }
    return mkdir(current.c_str(), mode);
}
}
