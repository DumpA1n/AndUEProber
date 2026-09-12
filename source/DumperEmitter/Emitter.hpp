#pragma once

#include "EmitterTypes.hpp"
#include "UpstreamEmitter.hpp"

namespace andueprober::dumper_emitter {

// Internal trusted-data boundary; these raw formatting fields are not an installed API.
Result emit(std::vector<ExtractedPackage::Struct>& records,
            std::vector<ExtractedPackage::Enum>& enums, const Limits& limits = {}) noexcept;

} // namespace andueprober::dumper_emitter
