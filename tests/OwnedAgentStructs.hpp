#pragma once
#include "OwnedStructs.hpp"
#include "OwnedAgentObjects.hpp"

struct OwnedAgentStructConfig {
    std::array<andueprober::StructSample, 3> metadata;
    std::array<andueprober::StructFieldSample, 3> fields;
    OwnedAgentObjectConfig foundation;
    std::array<AUEP_StructAnchor, 3> structAnchors;
    std::array<AUEP_FieldAnchor, 3> fieldAnchors;
    AUEP_StructProbeOptions options{};
    OwnedAgentStructConfig(const OwnedStructs& fixture, const std::string& modulePath,
        std::uintptr_t moduleAddress, const std::string& mode)
        : metadata(fixture.structSamples()), fields(fixture.fieldSamples()),
          foundation(fixture.phase1, modulePath, moduleAddress, mode) {
        for (std::size_t i = 0; i < structAnchors.size(); ++i) {
            const auto& value = metadata[i];
            structAnchors[i] = {value.object, value.expectedSuper, value.expectedFirstChild, value.identity.c_str(),
                value.superIdentity.c_str(), value.firstChildIdentity.c_str(), value.expectedPropertiesSize, 1,
                *value.expectedChildProperties, value.childPropertiesIdentity.c_str(), mode == "no-alignment" ? 0u : 1u, *value.expectedMinAlignment};
        }
        for (std::size_t i = 0; i < fieldAnchors.size(); ++i)
            fieldAnchors[i] = {fields[i].object, fields[i].expectedNext, fields[i].identity.c_str(), fields[i].nextIdentity.c_str()};
        options.struct_size = sizeof(options);
        options.indices = foundation.indices; options.indices.session_id = "owned-struct-agent";
        options.names = foundation.pool;
        options.metadata_id = "owned-compiled-struct-metadata-v1";
        options.struct_extent = sizeof(OwnedNativeStruct); options.field_extent = sizeof(OwnedNativeField);
        options.object_anchor_count = foundation.anchors.size(); options.struct_anchor_count = structAnchors.size();
        options.field_anchor_count = fieldAnchors.size(); options.object_anchors = foundation.anchors.data();
        options.struct_anchors = structAnchors.data(); options.field_anchors = fieldAnchors.data();
    }
    OwnedAgentStructConfig(const OwnedAgentStructConfig&) = delete;
    OwnedAgentStructConfig& operator=(const OwnedAgentStructConfig&) = delete;
};
