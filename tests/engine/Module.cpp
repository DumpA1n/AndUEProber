#include "Fixture.hpp"
#include <cstdlib>
#include <stdexcept>

namespace {
EngineFixtureObject* current = nullptr;
char16_t* allocate(std::uint32_t capacity) noexcept {
    if (current->mode == 6) return nullptr;
    if (std::this_thread::get_id() != current->owner) ++current->wrongThread;
    auto* data = static_cast<char16_t*>(std::malloc(capacity * sizeof(char16_t)));
    if (data) { ++current->allocations; ++current->outstanding; }
    return data;
}
void release(char16_t* data, std::uint32_t) noexcept {
    if (std::this_thread::get_id() != current->owner) ++current->wrongThread;
    std::free(data); ++current->releases; --current->outstanding;
    if (current->onRelease) current->onRelease(current->control);
}
andueprober::Error invoke(void* context, const andueprober::Utf16Sink* sink, andueprober::FStringValue* out) {
    current = static_cast<EngineFixtureObject*>(context);
    ++current->calls;
    if (std::this_thread::get_id() != current->owner) ++current->wrongThread;
    if (current->mode == 10) return andueprober::Error::None;
    auto* data = sink->allocate(sink->context, current->mode == 8 ? 65537 : 5);
    if (!data) return andueprober::Error::Internal;
    data[0] = u'A'; data[1] = 0xD83D; data[2] = 0xDE00; data[3] = u'Z'; data[4] = 0;
    *out = {data, 5, 5};
    switch (current->mode) {
    case 1: out->count = -1; break;
    case 2: out->capacity = 6; break;
    case 3: { static char16_t foreign[] = u"other"; out->data = foreign; break; }
    case 4: data[4] = u'X'; break;
    case 5: data[2] = u'X'; break;
    case 7: sink->allocate(sink->context, 5); break;
    case 9: throw std::runtime_error("owned shim failure");
    case 11: return andueprober::Error::PermissionDenied;
    case 12: out->count = 6; break;
    case 13: out->count = 0; break;
    default: break;
    }
    if (current->onInvoke) current->onInvoke(current->control);
    return andueprober::Error::None;
}
}
extern "C" __attribute__((visibility("default"))) void ownedEngineProvider(andueprober::CompiledTextProvider* result) {
    *result = {invoke, allocate, release};
}
