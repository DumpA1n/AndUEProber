#pragma once
#include "OwnedAgentFields.hpp"
#include "OwnedProperties.hpp"

struct OwnedAgentPropertyConfig {
    OwnedAgentFieldConfig foundation;
    std::array<andueprober::PropertySample, 3> metadata;
    std::array<AUEP_PropertyAnchor, 3> anchors;
    AUEP_PropertyProbeOptions options{};
    OwnedAgentPropertyConfig(const OwnedProperties& fixture, const std::string& modulePath,
        std::uintptr_t moduleAddress, const std::string& mode)
        : foundation(fixture.phase5, modulePath, moduleAddress, mode), metadata(fixture.samples()) {
        for (std::size_t i = 0; i < anchors.size(); ++i) {
            const auto& value = metadata[i];
            anchors[i] = {value.object, value.identity.c_str(), value.expectedArrayDim, value.expectedElementSize,
                value.expectedPropertyFlags, value.expectedOffsetInternal, value.containingValueSize, value.containingValueIdentity.c_str()};
        }
        options = {sizeof(options), foundation.options, "owned-compiled-property-metadata-v1",
            sizeof(OwnedNativeProperty), sizeof(OwnedNativeFField), static_cast<std::uint32_t>(anchors.size()), anchors.data()};
        options.fields.functions.classes.structures.indices.session_id = "owned-property-agent";
    }
    OwnedAgentPropertyConfig(const OwnedAgentPropertyConfig&) = delete;
    OwnedAgentPropertyConfig& operator=(const OwnedAgentPropertyConfig&) = delete;
};
