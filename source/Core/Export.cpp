#include "andueprober/Export.hpp"
#include "BuildInfo.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <ostream>
#include <streambuf>
#include <sys/file.h>
#include <unistd.h>

namespace andueprober {
namespace {
struct ExportInterrupted { Error error; };
void checkpoint(const ExportOptions& options) {
    if (options.cancelled && options.cancelled->load()) throw ExportInterrupted{Error::Cancelled};
    if (std::chrono::steady_clock::now() >= options.deadline)
        throw ExportInterrupted{Error::DeadlineExceeded};
}
void validateText(const std::string& text, const ExportOptions& options) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        checkpoint(options);
        auto end = begin + std::min<std::size_t>(text.size() - begin, 65536);
        if (end < text.size())
            while (end > begin && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
        if (end == begin || !validateUtf8(std::string_view(text).substr(begin, end - begin)))
            throw ExportInterrupted{Error::InvalidArgument};
        begin = end;
    }
    checkpoint(options);
}
class ManifestBuffer final : public std::streambuf {
public:
    explicit ManifestBuffer(const ExportOptions& options) : options_(options) {}
    const std::string& content() const { return content_; }
protected:
    std::streamsize xsputn(const char* input, std::streamsize size) override {
        checkpoint(options_);
        if (size < 0 || static_cast<std::size_t>(size) > options_.maximumManifestBytes - content_.size())
            throw ExportInterrupted{Error::BudgetExceeded};
        content_.append(input, static_cast<std::size_t>(size));
        return size;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const auto character = traits_type::to_char_type(value);
        xsputn(&character, 1);
        return value;
    }
private:
    const ExportOptions& options_;
    std::string content_;
};
bool safePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute()) return false;
    for (const auto& component : path)
        if (component == "." || component == ".." || component.empty()) return false;
    return path.string().find('\0') == std::string::npos;
}
struct Quoted { const std::string& value; };
Quoted quote(const std::string& value) { return {value}; }
std::ostream& operator<<(std::ostream& out, Quoted text) {
    constexpr char hex[] = "0123456789abcdef";
    out.put('"');
    for (unsigned char c : text.value) {
        if (c == '"' || c == '\\') { out.put('\\'); out.put(static_cast<char>(c)); }
        else if (c < 0x20) {
            const char escape[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
            out.write(escape, sizeof(escape));
        } else out.put(static_cast<char>(c));
    }
    return out.put('"');
}
std::uint64_t checksum(const std::string& content, const ExportOptions& options) {
    std::uint64_t value = 14695981039346656037ULL;
    for (std::size_t index = 0; index < content.size(); ++index) {
        if ((index & 65535) == 0) checkpoint(options);
        value = (value ^ static_cast<unsigned char>(content[index])) * 1099511628211ULL;
    }
    return value;
}
struct FileDescriptor {
    int value = -1;
    ~FileDescriptor() { if (value >= 0) ::close(value); }
};
struct CompletionMarker {
    const std::filesystem::path& complete;
    const std::filesystem::path& pending;
    const std::filesystem::path& directory;
    bool& uncertain;
    bool active = true;
    void retract() noexcept {
        if (!active) return;
        active = false;
        if (::rename(complete.c_str(), pending.c_str()) != 0) { uncertain = true; return; }
        FileDescriptor fd{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
        if (fd.value < 0) { uncertain = true; return; }
        if (::fsync(fd.value) != 0) uncertain = true;
        const auto descriptor = fd.value;
        fd.value = -1;
        if (::close(descriptor) != 0) uncertain = true;
    }
    ~CompletionMarker() { retract(); }
};
}
ExportResult publishExport(const Snapshot& snapshot, const std::vector<ExportFile>& files, const ExportOptions& options) {
    ExportResult result;
    try {
        auto fail = [&](Status status) { result.status = std::move(status); return result; };
        auto check = [&](ExportOperation operation, const std::filesystem::path& path) -> Status {
            try {
                checkpoint(options);
                if (options.beforeOperation) {
                    auto status = options.beforeOperation(operation, path);
                    if (!status) return status;
                }
                checkpoint(options);
                return {};
            } catch (const ExportInterrupted& error) {
                return {error.error, {}};
            } catch (...) {
                return {Error::Internal, {}};
            }
        };
        auto ioError = [] { return Status{Error::Io, std::strerror(errno)}; };
        checkpoint(options);
        if (!options.maximumFiles || !options.maximumFileBytes || !options.maximumTotalBytes ||
            !options.maximumManifestBytes || !options.maximumMetadataEntries || !options.maximumPathBytes)
            return fail({Error::InvalidArgument, "Export limits must be nonzero"});
        if (files.size() > options.maximumFiles) return fail({Error::BudgetExceeded, "Export file count exceeds its limit"});
        if (snapshot.sessionId.size() > options.maximumPathBytes)
            return fail({Error::BudgetExceeded, "Export session path exceeds its limit"});
        auto entries = options.maximumMetadataEntries;
        auto metadataBytes = options.maximumManifestBytes;
        auto consume = [&](std::size_t amount, std::size_t& remaining) {
            checkpoint(options);
            if (amount > remaining) throw ExportInterrupted{Error::BudgetExceeded};
            remaining -= amount;
        };
        auto text = [&](const std::string& value) { consume(value.size(), metadataBytes); validateText(value, options); };
        text(snapshot.sessionId); text(snapshot.moduleIdentity); text(snapshot.layoutIdentity); text(options.toolRevision);
        consume(snapshot.offsets.size(), entries);
        consume(options.dependencyRevisions.size(), entries);
        for (const auto& [name, revision] : options.dependencyRevisions) { text(name); text(revision); }
        for (const auto& [name, offset] : snapshot.offsets) {
            text(name);
            consume(offset.dependencies.size(), entries);
            consume(offset.evidence.size(), entries);
            for (const auto& [dependency, version] : offset.dependencies) { (void)version; text(dependency); }
            for (const auto& evidence : offset.evidence) {
                text(evidence.check); text(evidence.source);
                consume(evidence.relativeAddresses.size(), entries);
                consume(evidence.sampleIdentities.size(), entries);
                for (const auto& identity : evidence.sampleIdentities) text(identity);
            }
        }
        if (auto status = validateSnapshot(snapshot); !status) return fail(status);
        if (!options.root.is_absolute() || !safePath(snapshot.sessionId) || snapshot.sessionId.starts_with('.') ||
            std::filesystem::path(snapshot.sessionId).has_parent_path() || files.empty())
            return fail({Error::InvalidArgument, "Export requires an absolute root, single-component session ID and files"});
        std::set<std::string> paths;
        auto fileBytes = options.maximumTotalBytes;
        for (const auto& file : files) {
            checkpoint(options);
            if (file.relativePath.size() > options.maximumPathBytes || file.content.size() > options.maximumFileBytes)
                return fail({Error::BudgetExceeded, "Export path or individual file exceeds its limit"});
            consume(file.content.size(), fileBytes);
            validateText(file.relativePath, options);
            if (!safePath(file.relativePath) || file.relativePath == "completion.json" ||
                file.relativePath == ".manifest.pending" || !paths.insert(file.relativePath).second)
                return fail({Error::InvalidArgument, "Export contains an invalid, duplicate or reserved file path"});
        }
        std::error_code error;
        if (auto status = check(ExportOperation::CreateDirectory, options.root); !status) return fail(status);
        std::filesystem::create_directories(options.root, error);
        if (error) return fail({Error::Io, error.message()});
        auto finalDirectory = options.root / snapshot.sessionId;
        FileDescriptor lock{::open((options.root / ".publish.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)};
        if (lock.value < 0) return fail(ioError());
        if (flock(lock.value, LOCK_EX | LOCK_NB) != 0)
            return fail(errno == EWOULDBLOCK ? Status{Error::Busy, "Export root is locked by another publisher"} : ioError());
        if (std::filesystem::exists(finalDirectory, error) || error)
            return fail({Error::Io, "Export session directory already exists or cannot be inspected"});
        std::string temporary = (options.root / (".staging-" + snapshot.sessionId + "-XXXXXX")).string();
        if (!mkdtemp(temporary.data())) return fail(ioError());
        result.incompleteDirectory = temporary;
        auto writeFile = [&](const std::filesystem::path& path, const std::string& content) -> Status {
            if (auto status = check(ExportOperation::CreateDirectory, path.parent_path()); !status) return status;
            std::filesystem::create_directories(path.parent_path(), error);
            if (error) return {Error::Io, error.message()};
            if (auto status = check(ExportOperation::Open, path); !status) return status;
            FileDescriptor fd{::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600)};
            if (fd.value < 0) return ioError();
            std::size_t offset = 0;
            while (offset < content.size()) {
                if (auto status = check(ExportOperation::Write, path); !status) return status;
                auto count = ::write(fd.value, content.data() + offset, std::min<std::size_t>(content.size() - offset, 65536));
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) return ioError();
                offset += static_cast<std::size_t>(count);
            }
            if (auto status = check(ExportOperation::Flush, path); !status) return status;
            if (::fsync(fd.value) != 0) return ioError();
            if (auto status = check(ExportOperation::Close, path); !status) return status;
            int descriptor = fd.value;
            fd.value = -1;
            if (::close(descriptor) != 0) return ioError();
            return {};
        };
        ManifestBuffer manifestBuffer(options);
        std::ostream manifest(&manifestBuffer);
        manifest.exceptions(std::ios::badbit | std::ios::failbit);
        manifest << "{\"schemaVersion\":" << snapshot.schemaVersion << ",\"sessionId\":" << quote(snapshot.sessionId)
            << ",\"moduleIdentity\":" << quote(snapshot.moduleIdentity) << ",\"generation\":" << snapshot.generation
            << ",\"toolRevision\":" << quote(options.toolRevision.empty() ? build::revision : options.toolRevision)
            << ",\"dependencyRevisions\":{";
        bool first = true;
        for (const auto& [name, revision] : options.dependencyRevisions) {
            if (!first) manifest << ',';
            first = false;
            manifest << quote(name) << ':' << quote(revision);
        }
        manifest << '}'
            << ",\"layout\":" << quote(snapshot.layout == Layout::UProperty ? "UProperty" : "FField")
            << ",\"layoutIdentity\":" << quote(snapshot.layoutIdentity)
            << ",\"checksumAlgorithm\":\"fnv1a64\",\"offsets\":{";
        first = true;
        for (const auto& [name, offset] : snapshot.offsets) {
            if (!first) manifest << ',';
            first = false;
            const char* origin = offset.origin == Origin::User ? "user" : offset.origin == Origin::Profile ? "profile" : "probe";
            manifest << quote(name) << ":{\"value\":" << *offset.value << ",\"origin\":" << quote(origin)
                << ",\"validation\":\"validated\",\"version\":" << offset.version << ",\"dependencies\":{";
            bool firstDependency = true;
            for (const auto& [dependency, version] : offset.dependencies) {
                if (!firstDependency) manifest << ',';
                firstDependency = false;
                manifest << quote(dependency) << ':' << version;
            }
            manifest << "},\"evidence\":[";
            bool firstEvidence = true;
            for (const auto& evidence : offset.evidence) {
                if (!firstEvidence) manifest << ',';
                firstEvidence = false;
                manifest << "{\"check\":" << quote(evidence.check) << ",\"passed\":true,\"samples\":" << evidence.samples << ",\"relativeAddresses\":[";
                for (std::size_t index = 0; index < evidence.relativeAddresses.size(); ++index) {
                    if (index) manifest << ',';
                    manifest << evidence.relativeAddresses[index];
                }
                manifest << "],\"source\":" << quote(evidence.source) << ",\"sampleIdentities\":[";
                for (std::size_t index = 0; index < evidence.sampleIdentities.size(); ++index) {
                    if (index) manifest << ',';
                    manifest << quote(evidence.sampleIdentities[index]);
                }
                manifest << "]}";
            }
            manifest << "]}";
        }
        manifest << "},\"files\":[";
        first = true;
        for (const auto& file : files) {
            if (auto status = writeFile(result.incompleteDirectory / file.relativePath, file.content); !status) return fail(status);
            if (!first) manifest << ',';
            first = false;
            manifest << "{\"path\":" << quote(file.relativePath) << ",\"bytes\":" << file.content.size()
                << ",\"checksum\":" << quote(std::to_string(checksum(file.content, options))) << '}';
        }
        manifest << "],\"complete\":true}\n";
        if (auto status = writeFile(result.incompleteDirectory / ".manifest.pending", manifestBuffer.content()); !status) return fail(status);
        auto syncDirectory = [&](const std::filesystem::path& path) -> Status {
            if (auto status = check(ExportOperation::Flush, path); !status) return status;
            FileDescriptor directory{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
            if (directory.value < 0 || ::fsync(directory.value) != 0) return ioError();
            if (auto status = check(ExportOperation::Close, path); !status) return status;
            const auto descriptor = directory.value;
            directory.value = -1;
            if (::close(descriptor) != 0) return ioError();
            try { checkpoint(options); }
            catch (const ExportInterrupted& interrupted) { return {interrupted.error, {}}; }
            return {};
        };
        std::vector<std::filesystem::path> directories{result.incompleteDirectory};
        for (std::filesystem::recursive_directory_iterator item(result.incompleteDirectory, error), end; item != end && !error; item.increment(error)) {
            checkpoint(options);
            const bool isDirectory = item->is_directory(error);
            if (error) return fail({Error::Io, error.message()});
            if (isDirectory) directories.push_back(item->path());
        }
        if (error) return fail({Error::Io, error.message()});
        for (auto it = directories.rbegin(); it != directories.rend(); ++it)
            if (auto status = syncDirectory(*it); !status) return fail(status);
        if (auto status = check(ExportOperation::Publish, finalDirectory); !status) return fail(status);
        std::filesystem::rename(result.incompleteDirectory, finalDirectory, error);
        if (error) {
            return fail({Error::Io, error.message()});
        }
        // The completion marker is finalized only after the session directory is published.
        result.incompleteDirectory = finalDirectory;
        if (auto status = syncDirectory(options.root); !status) return fail(status);
        if (auto status = check(ExportOperation::Finalize, finalDirectory); !status) return fail(status);
        const auto pendingMarker = finalDirectory / ".manifest.pending";
        const auto completeMarker = finalDirectory / "completion.json";
        std::filesystem::rename(pendingMarker, completeMarker, error);
        if (error) return fail({Error::Io, error.message()});
        CompletionMarker marker{completeMarker, pendingMarker, finalDirectory, result.completionUncertain};
        if (auto status = syncDirectory(finalDirectory); !status) {
            marker.retract();
            return fail(status);
        }
        result.publishedDirectory = finalDirectory;
        result.incompleteDirectory.clear();
        marker.active = false;
        return result;
    } catch (const ExportInterrupted& error) {
        result.status.code = error.error;
    } catch (const std::filesystem::filesystem_error&) {
        result.status.code = Error::Io;
    } catch (...) {
        result.status.code = Error::Internal;
    }
    result.publishedDirectory.clear();
    result.status.message.clear();
    return result;
}
bool isCompletedExport(const std::filesystem::path& directory) {
    try {
        std::error_code error;
        const auto resolved = std::filesystem::canonical(directory, error);
        if (error || resolved.filename().empty() || resolved.filename().string().starts_with('.')) return false;
        const auto marker = std::filesystem::symlink_status(resolved / "completion.json", error);
        return !error && std::filesystem::is_regular_file(marker);
    } catch (...) { return false; }
}
}
