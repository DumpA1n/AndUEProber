#include <andueprober/Export.hpp>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace andueprober;
using Clock = std::chrono::steady_clock;
namespace {
unsigned checks = 0;
std::atomic<bool>* cancelAfterFlush = nullptr;
Clock::time_point returnAfterFlush = Clock::time_point::min();
bool finalFlushObserved = false;
unsigned failDirectoryCloses = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
Snapshot sample(std::string id) {
    Snapshot result;
    result.sessionId = std::move(id);
    result.moduleIdentity = "owned-export-input";
    result.layoutIdentity = "owned-field-layout";
    result.layout = Layout::FField;
    result.generation = 1;
    Offset index;
    index.value = 0;
    index.validation = Validation::Validated;
    index.evidence.push_back({"owned index samples", true, 2, {0, 4}, "fixture", {"first", "second"}});
    CHECK(publishOffset(result, "Index", std::move(index)));
    CHECK(validateSnapshot(result));
    return result;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}
// The fixture performs the real filesystem flush, then controls when its result
// becomes visible to the exporter. This exercises cancellation after a kernel call.
extern "C" int fsync(int descriptor) {
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    const auto result = static_cast<int>(syscall(SYS_fsync, descriptor));
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
    if (cancelAfterFlush) {
        cancelAfterFlush->store(true);
        cancelAfterFlush = nullptr;
        finalFlushObserved = true;
    }
    if (returnAfterFlush != Clock::time_point::min()) {
        std::this_thread::sleep_until(returnAfterFlush);
        returnAfterFlush = Clock::time_point::min();
        finalFlushObserved = true;
    }
    return result;
}
// The descriptor is actually closed; the fixture then reports a directory close
// error to verify that publication and marker retraction propagate that result.
extern "C" int close(int descriptor) {
    struct stat info{};
    const bool directory = failDirectoryCloses && ::fstat(descriptor, &info) == 0 && S_ISDIR(info.st_mode);
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    const auto result = static_cast<int>(syscall(SYS_close, descriptor));
#if defined(__APPLE__)
#pragma clang diagnostic pop
#endif
    if (result == 0 && directory) {
        --failDirectoryCloses;
        errno = EIO;
        return -1;
    }
    return result;
}
int main() {
#if defined(__ANDROID__)
    auto temporary = std::string("/data/local/tmp/andueprober-export-limits-XXXXXX");
#else
    auto temporary = (std::filesystem::temp_directory_path() / "andueprober-export-limits-XXXXXX").string();
#endif
    CHECK(mkdtemp(temporary.data()));
    const std::filesystem::path root = temporary;
    const std::vector<ExportFile> files{{"data.txt", "abcd"}};
    ExportOptions baseline{root / "published"};
    auto good = publishExport(sample("previous"), files, baseline);
    CHECK(good.status && !good.completionUncertain && isCompletedExport(good.publishedDirectory));
    const auto previous = contents(good.publishedDirectory / "completion.json");
    auto rejectBeforeIo = [&](ExportOptions options, std::vector<ExportFile> input, Error expected) {
        unsigned operations = 0;
        options.root = root / ("preflight-" + std::to_string(checks));
        options.beforeOperation = [&](ExportOperation, const auto&) { ++operations; return Status{}; };
        const auto result = publishExport(sample("limits"), input, options);
        CHECK(result.status.code == expected);
        CHECK(result.publishedDirectory.empty() && result.incompleteDirectory.empty());
        CHECK(!std::filesystem::exists(options.root) && operations == 0);
    };
    ExportOptions options;
    options.maximumFiles = 1;
    rejectBeforeIo(options, {files[0], {"second.txt", "a"}}, Error::BudgetExceeded);
    options = {}; options.maximumFileBytes = 3;
    rejectBeforeIo(options, files, Error::BudgetExceeded);
    options = {}; options.maximumTotalBytes = 3;
    rejectBeforeIo(options, {{"one", "ab"}, {"two", "cd"}}, Error::BudgetExceeded);
    options = {}; options.maximumPathBytes = 6;
    rejectBeforeIo(options, files, Error::BudgetExceeded);
    options = {}; options.maximumPathBytes = 3;
    rejectBeforeIo(options, files, Error::BudgetExceeded);
    options = {}; options.maximumMetadataEntries = 2;
    rejectBeforeIo(options, files, Error::BudgetExceeded);
    options = {}; options.maximumManifestBytes = 3;
    rejectBeforeIo(options, files, Error::BudgetExceeded);
    options = {}; options.maximumFiles = 0;
    rejectBeforeIo(options, files, Error::InvalidArgument);
    options = {}; options.deadline = Clock::now();
    rejectBeforeIo(options, files, Error::DeadlineExceeded);
    std::atomic<bool> cancelled{true};
    options = {}; options.cancelled = &cancelled;
    rejectBeforeIo(options, files, Error::Cancelled);
    cancelled = false;

    options = {}; options.root = root / "bounded";
    options.maximumFiles = 1; options.maximumFileBytes = 4; options.maximumTotalBytes = 4;
    auto exact = publishExport(sample("exact"), files, options);
    CHECK(exact.status && contents(exact.publishedDirectory / "data.txt") == "abcd");
    options.maximumFileBytes = 1; options.maximumTotalBytes = 1;
    auto empty = publishExport(sample("empty"), {{"empty.txt", ""}}, options);
    CHECK(empty.status && std::filesystem::file_size(empty.publishedDirectory / "empty.txt") == 0);

    options = {}; options.root = root / "escaped"; options.maximumManifestBytes = 512;
    auto escapedInput = sample("escaping");
    escapedInput.moduleIdentity.assign(200, '\n');
    auto escaped = publishExport(escapedInput, files, options);
    CHECK(escaped.status.code == Error::BudgetExceeded && !escaped.incompleteDirectory.empty());
    CHECK(!isCompletedExport(escaped.incompleteDirectory) && escaped.publishedDirectory.empty());
    CHECK(!std::filesystem::exists(escaped.incompleteDirectory / ".manifest.pending"));

    for (const auto* invalid : {"\xff", "\xc0\xaf", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80"})
        CHECK(validateUtf8(invalid).code == Error::InvalidArgument);
    CHECK(validateUtf8("ASCII\xf0\x9f\x98\x80\xe4\xb8\xad"));
    for (unsigned field = 0; field < 12; ++field) {
        options = {}; options.root = root / ("invalid-utf8-" + std::to_string(field));
        auto input = sample("metadata");
        auto data = files;
        const std::string invalid(1, static_cast<char>(0xff));
        switch (field) {
            case 0: input.moduleIdentity = invalid; break;
            case 1: input.layoutIdentity = invalid; break;
            case 2: input.sessionId = invalid; break;
            case 3: {
                auto node = input.offsets.extract(input.offsets.begin()); node.key() = invalid;
                input.offsets.insert(std::move(node)); break;
            }
            case 4: input.offsets.begin()->second.evidence[0].check = invalid; break;
            case 5: input.offsets.begin()->second.evidence[0].source = invalid; break;
            case 6: input.offsets.begin()->second.evidence[0].sampleIdentities[0] = invalid; break;
            case 7: options.toolRevision = invalid; break;
            case 8: options.dependencyRevisions[invalid] = "revision"; break;
            case 9: options.dependencyRevisions["producer"] = invalid; break;
            case 10: data[0].relativePath = invalid; break;
            case 11: input.offsets.begin()->second.dependencies[invalid] = 1; break;
        }
        const auto result = publishExport(input, data, options);
        CHECK(result.status.code == Error::InvalidArgument && result.publishedDirectory.empty());
        CHECK(!std::filesystem::exists(options.root));
    }
    options = {}; options.root = root / "unicode";
    auto unicode = sample("valid-utf8");
    unicode.moduleIdentity = std::string(65535, 'a') + "\xf0\x9f\x98\x80";
    unicode.layoutIdentity = std::string("valid\0text", 10);
    const auto encoded = publishExport(unicode, files, options);
    CHECK(encoded.status);
    const auto manifest = contents(encoded.publishedDirectory / "completion.json");
    CHECK(manifest.find("\xf0\x9f\x98\x80") != std::string::npos);
    CHECK(manifest.find("valid\\u0000text") != std::string::npos && validateUtf8(manifest));

    for (auto operation : {ExportOperation::CreateDirectory, ExportOperation::Open, ExportOperation::Write,
                          ExportOperation::Flush, ExportOperation::Close, ExportOperation::Publish, ExportOperation::Finalize}) {
        options = {}; options.root = baseline.root;
        options.beforeOperation = [operation](ExportOperation current, const auto&) -> Status {
            if (operation == current) throw std::runtime_error("Owned operation failure");
            return {};
        };
        auto result = publishExport(sample("throw-" + std::to_string(static_cast<int>(operation))), files, options);
        CHECK(result.status.code == Error::Internal && result.publishedDirectory.empty());
        CHECK(result.incompleteDirectory.empty() || !isCompletedExport(result.incompleteDirectory));
        CHECK(contents(good.publishedDirectory / "completion.json") == previous);
    }

    options = {}; options.root = root / "callback-cancel"; options.cancelled = &cancelled;
    options.beforeOperation = [&](ExportOperation operation, const auto&) {
        if (operation == ExportOperation::Publish) cancelled = true;
        return Status{};
    };
    auto lateCancel = publishExport(sample("cancelled"), files, options);
    CHECK(lateCancel.status.code == Error::Cancelled && lateCancel.publishedDirectory.empty());
    CHECK(!std::filesystem::exists(options.root / "cancelled"));
    cancelled = false;

    options = {}; options.root = root / "deadline"; options.deadline = Clock::now() + std::chrono::milliseconds(10);
    options.beforeOperation = [](ExportOperation operation, const auto&) {
        if (operation == ExportOperation::CreateDirectory) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return Status{};
    };
    auto deadline = publishExport(sample("deadline"), files, options);
    CHECK(deadline.status.code == Error::DeadlineExceeded && !std::filesystem::exists(options.root));

    options = {}; options.root = root / "partial-write"; options.cancelled = &cancelled;
    unsigned writes = 0;
    options.beforeOperation = [&](ExportOperation operation, const auto&) {
        if (operation == ExportOperation::Write && ++writes == 2) cancelled = true;
        return Status{};
    };
    auto partial = publishExport(sample("partial"), {{"large.bin", std::string(3 * 65536, 'x')}}, options);
    CHECK(partial.status.code == Error::Cancelled && writes == 2);
    CHECK(std::filesystem::file_size(partial.incompleteDirectory / "large.bin") == 65536);
    CHECK(!isCompletedExport(partial.incompleteDirectory));
    cancelled = false;

    for (bool preventRetraction : {false, true}) {
        options = {}; options.root = root / "finalization";
        const auto id = preventRetraction ? "uncertain" : "retracted";
        options.beforeOperation = [&](ExportOperation operation, const auto& path) -> Status {
            if (operation == ExportOperation::Flush && path == options.root / id &&
                std::filesystem::exists(path / "completion.json")) {
                if (preventRetraction) std::filesystem::create_directory(path / ".manifest.pending");
                throw std::runtime_error("Owned final marker flush failure");
            }
            return {};
        };
        auto result = publishExport(sample(id), files, options);
        CHECK(result.status.code == Error::Internal && result.publishedDirectory.empty());
        CHECK(result.completionUncertain == preventRetraction);
        CHECK(std::filesystem::is_regular_file(result.incompleteDirectory / "completion.json") == preventRetraction);
        CHECK(isCompletedExport(result.incompleteDirectory) == preventRetraction);
        if (!preventRetraction) CHECK(std::filesystem::is_regular_file(result.incompleteDirectory / ".manifest.pending"));
    }
    for (unsigned failures : {1U, 2U}) {
        options = {}; options.root = root / "directory-close";
        const auto id = "close-" + std::to_string(failures);
        options.beforeOperation = [&](ExportOperation operation, const auto& path) {
            if (operation == ExportOperation::Close && path == options.root / id &&
                std::filesystem::exists(path / "completion.json")) failDirectoryCloses = failures;
            return Status{};
        };
        const auto result = publishExport(sample(id), files, options);
        CHECK(failDirectoryCloses == 0);
        CHECK(result.status.code == Error::Io && result.publishedDirectory.empty());
        CHECK(result.completionUncertain == (failures == 2));
        CHECK(!isCompletedExport(result.incompleteDirectory));
        CHECK(std::filesystem::is_regular_file(result.incompleteDirectory / ".manifest.pending"));
    }
    for (bool expire : {false, true}) {
        cancelled = false; finalFlushObserved = false;
        options = {}; options.root = root / "post-flush"; options.cancelled = &cancelled;
        options.deadline = Clock::now() + std::chrono::seconds(5);
        const auto id = expire ? "deadline" : "cancel";
        options.beforeOperation = [&](ExportOperation operation, const auto& path) {
            if (operation == ExportOperation::Flush && path == options.root / id &&
                std::filesystem::exists(path / "completion.json")) {
                if (expire) returnAfterFlush = options.deadline + std::chrono::milliseconds(1);
                else cancelAfterFlush = &cancelled;
            }
            return Status{};
        };
        auto result = publishExport(sample(id), files, options);
        CHECK(finalFlushObserved);
        CHECK(result.status.code == (expire ? Error::DeadlineExceeded : Error::Cancelled));
        CHECK(result.publishedDirectory.empty() && !result.completionUncertain);
        CHECK(!isCompletedExport(result.incompleteDirectory));
        CHECK(std::filesystem::is_regular_file(result.incompleteDirectory / ".manifest.pending"));
    }
    CHECK(contents(good.publishedDirectory / "completion.json") == previous);
    const auto stage = root / ".staging-reader";
    std::filesystem::rename(good.publishedDirectory, stage);
    CHECK(!isCompletedExport(stage));
    CHECK(!isCompletedExport(stage / ""));
    CHECK(!isCompletedExport(stage / "."));
    std::filesystem::create_directory_symlink(stage, root / "stage-alias");
    CHECK(!isCompletedExport(root / "stage-alias"));
    std::filesystem::create_directory(root / "linked-marker");
    std::filesystem::create_symlink(stage / "completion.json", root / "linked-marker/completion.json");
    CHECK(!isCompletedExport(root / "linked-marker"));
    std::filesystem::remove_all(root);
    std::printf("PASS: %u export checks; bounded files/metadata/manifest, deadline, cancellation, operation and close failures, completion retraction\n", checks);
}
