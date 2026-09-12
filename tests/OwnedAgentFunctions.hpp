#pragma once
#include "OwnedAgentClasses.hpp"
#include "OwnedFunctions.hpp"

struct OwnedAgentFunctionConfig {
    OwnedAgentClassConfig foundation;
    std::array<andueprober::FunctionSample, 3> metadata;
    std::array<AUEP_FunctionAnchor, 3> anchors;
    AUEP_FunctionProbeOptions options{};
    OwnedAgentFunctionConfig(const OwnedFunctions& fixture, const std::string& modulePath,
        std::uintptr_t moduleAddress, const std::string& mode)
        : foundation(fixture.phase3, modulePath, moduleAddress, mode), metadata(fixture.samples()) {
        for (std::size_t i = 0; i < anchors.size(); ++i) {
            const auto& value = metadata[i];
            anchors[i] = {value.object, value.identity.c_str(), value.expectedFlags, value.expectedNumParms,
                value.expectedParmsSize, value.expectedReturnOffset, value.expectedNativeFunction, value.nativeFunctionIdentity.c_str()};
        }
        options = {sizeof(options), foundation.options, "owned-compiled-function-metadata-v1",
            sizeof(OwnedNativeFunction), static_cast<std::uint32_t>(anchors.size()), anchors.data()};
        options.classes.structures.indices.session_id = "owned-function-agent";
    }
    OwnedAgentFunctionConfig(const OwnedAgentFunctionConfig&) = delete;
    OwnedAgentFunctionConfig& operator=(const OwnedAgentFunctionConfig&) = delete;
};
