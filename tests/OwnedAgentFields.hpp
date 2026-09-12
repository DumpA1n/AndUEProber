#pragma once
#include "OwnedAgentFunctions.hpp"
#include "OwnedFields.hpp"

struct OwnedAgentFieldConfig {
    OwnedAgentFunctionConfig foundation;
    std::array<andueprober::FieldBaseSample, 3> metadata;
    std::array<AUEP_FieldBaseAnchor, 3> anchors;
    AUEP_FieldBaseProbeOptions options{};
    OwnedAgentFieldConfig(const OwnedFields& fixture, const std::string& modulePath,
        std::uintptr_t moduleAddress, const std::string& mode)
        : foundation(fixture.phase4, modulePath, moduleAddress, mode), metadata(fixture.samples(mode == "reverse")) {
        for (std::size_t i = 0; i < anchors.size(); ++i) {
            const auto& value = metadata[i];
            anchors[i] = {value.object, value.identity.c_str(), value.expectedName.c_str(), value.expectedOwner,
                value.ownerIdentity.c_str(), static_cast<std::uint32_t>(value.ownerIsUObject), value.expectedNext,
                value.nextIdentity.c_str(), value.expectedClass, value.classIdentity.c_str(), value.expectedFlags};
        }
        const auto profile = fixture.profile(mode == "reverse");
        options = {sizeof(options), foundation.options, "owned-compiled-ffield-metadata-v1", profile.extent,
            static_cast<std::uint32_t>(anchors.size()), 1, profile.ownerLayout.pointerOffset,
            profile.ownerLayout.kindOffset, profile.ownerLayout.size, anchors.data()};
        options.functions.classes.structures.indices.session_id = "owned-field-base-agent";
    }
    OwnedAgentFieldConfig(const OwnedAgentFieldConfig&) = delete;
    OwnedAgentFieldConfig& operator=(const OwnedAgentFieldConfig&) = delete;
};
