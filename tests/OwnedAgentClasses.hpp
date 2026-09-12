#pragma once
#include "OwnedAgentStructs.hpp"
#include "OwnedClasses.hpp"

struct OwnedAgentClassConfig {
    OwnedAgentStructConfig foundation;
    std::array<andueprober::ClassSample, 3> metadata;
    std::array<AUEP_ClassAnchor, 3> anchors;
    AUEP_ClassProbeOptions options{};
    OwnedAgentClassConfig(const OwnedClasses& fixture, const std::string& modulePath,
        std::uintptr_t moduleAddress, const std::string& mode)
        : foundation(fixture.phase2, modulePath, moduleAddress, mode), metadata(fixture.samples()) {
        for (std::size_t i = 0; i < anchors.size(); ++i) {
            const auto& value = metadata[i];
            anchors[i] = {value.object, value.identity.c_str(), value.expectedCastFlags,
                value.expectedDefaultObject, value.defaultObjectIdentity.c_str()};
        }
        options = {sizeof(options), foundation.options, "owned-compiled-class-metadata-v1",
            sizeof(OwnedNativeClass), static_cast<std::uint32_t>(anchors.size()), anchors.data()};
        options.structures.indices.session_id = "owned-class-agent";
    }
    OwnedAgentClassConfig(const OwnedAgentClassConfig&) = delete;
    OwnedAgentClassConfig& operator=(const OwnedAgentClassConfig&) = delete;
};
