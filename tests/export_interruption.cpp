#include <andueprober/Export.hpp>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/wait.h>
#include <unistd.h>

using namespace andueprober;
namespace {
unsigned checks = 0;
#define CHECK(value) do { ++checks; if (!(value)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); std::abort(); } } while (false)
Snapshot sample(std::string id) {
    Snapshot value;
    value.sessionId = std::move(id);
    value.moduleIdentity = "owned-interruption-input";
    value.layoutIdentity = "owned-field-layout";
    value.layout = Layout::FField;
    value.generation = 1;
    Offset offset;
    offset.value = 0;
    offset.validation = Validation::Validated;
    offset.evidence.push_back({"owned compiled offset", true, 2, {0, 4}, "fixture", {"first", "second"}});
    CHECK(publishOffset(value, "Index", std::move(offset)));
    CHECK(validateSnapshot(value));
    return value;
}
std::string contents(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(stream.good());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
enum class Boundary { PartialWrite, Staged, PublishedDirectory, VisibleMarker };
void interrupted(const std::filesystem::path& root, Boundary boundary) {
    const std::vector<ExportFile> files{{"nested/data.bin", std::string(131072, 'x')}};
    const auto previous = publishExport(sample("previous"), {{"previous.bin", "preserved"}}, ExportOptions{root});
    CHECK(previous.status && isCompletedExport(previous.publishedDirectory));
    const auto manifest = contents(previous.publishedDirectory / "completion.json");
    const auto input = sample("interrupted");
    const auto child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        alarm(10);
        unsigned writes = 0;
        ExportOptions options{root};
        options.beforeOperation = [&](ExportOperation operation, const auto& path) -> Status {
            const bool partial = boundary == Boundary::PartialWrite && operation == ExportOperation::Write &&
                path.filename() == "data.bin" && ++writes == 2;
            const bool staged = boundary == Boundary::Staged && operation == ExportOperation::Publish;
            const bool published = boundary == Boundary::PublishedDirectory && operation == ExportOperation::Finalize;
            const bool marker = boundary == Boundary::VisibleMarker && operation == ExportOperation::Flush &&
                path == root / "interrupted" && std::filesystem::exists(path / "completion.json");
            // _exit terminates without C++ destructors or the export marker guard.
            if (partial || staged || published || marker) _exit(42);
            return {};
        };
        (void)publishExport(input, files, options);
        _exit(97);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    CHECK(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 42);
    const bool directoryPublished = boundary == Boundary::PublishedDirectory || boundary == Boundary::VisibleMarker;
    auto incomplete = root / "interrupted";
    if (!directoryPublished) {
        unsigned stages = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.path().filename().string().starts_with(".staging-interrupted-")) {
                incomplete = entry.path();
                ++stages;
            }
        }
        CHECK(stages == 1);
        CHECK(!std::filesystem::exists(root / "interrupted"));
    }
    CHECK(std::filesystem::is_directory(incomplete));
    CHECK(std::filesystem::file_size(incomplete / "nested/data.bin") ==
        (boundary == Boundary::PartialWrite ? 65536 : 131072));
    const bool visible = boundary == Boundary::VisibleMarker;
    CHECK(std::filesystem::exists(incomplete / "completion.json") == visible);
    CHECK(isCompletedExport(incomplete) == visible);
    CHECK(std::filesystem::exists(incomplete / ".manifest.pending") ==
        (boundary == Boundary::Staged || boundary == Boundary::PublishedDirectory));
    CHECK(contents(previous.publishedDirectory / "completion.json") == manifest);
    CHECK(contents(previous.publishedDirectory / "previous.bin") == "preserved");
    const auto recovered = publishExport(sample("recovered"), {{"recovered.bin", "complete"}}, ExportOptions{root});
    CHECK(recovered.status && isCompletedExport(recovered.publishedDirectory));
    CHECK(std::filesystem::is_directory(incomplete));
}
}
int main() {
#if defined(__ANDROID__)
    auto temporary = std::string("/data/local/tmp/andueprober-export-interruption-XXXXXX");
#else
    auto temporary = (std::filesystem::temp_directory_path() / "andueprober-export-interruption-XXXXXX").string();
#endif
    CHECK(mkdtemp(temporary.data()));
    const std::filesystem::path root = temporary;
    for (const auto boundary : {Boundary::PartialWrite, Boundary::Staged, Boundary::PublishedDirectory, Boundary::VisibleMarker})
        interrupted(root / std::to_string(static_cast<unsigned>(boundary)), boundary);
    CHECK(std::filesystem::remove_all(root) > 0);
    std::printf("PASS: %u interruption checks across 4 process-exit boundaries; marker presence does not establish durability\n", checks);
}
