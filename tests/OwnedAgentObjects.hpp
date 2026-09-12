#pragma once
#include "OwnedRelations.hpp"
#include "andueprober/Agent.h"

struct OwnedAgentObjectConfig {
    std::array<andueprober::NameSample, 2> names, classes;
    std::array<andueprober::PointerSample, 2> outers;
    std::array<AUEP_ObjectAnchor, 2> anchors;
    AUEP_ObjectIndexOptions indices{};
    AUEP_NamePoolOptions pool{};
    OwnedAgentObjectConfig(const OwnedRelations& fixture, const std::string& modulePath,
        std::uintptr_t moduleAddress, const std::string& mode)
        : names(fixture.names()), classes(fixture.classes()), outers(fixture.outers()) {
        for (std::size_t i = 0; i < anchors.size(); ++i)
            anchors[i] = {names[i].object, outers[i].expected, names[i].identity.c_str(), names[i].expected.c_str(),
                classes[i].expected.c_str(), outers[i].expectedIdentity.c_str()};
        indices.struct_size = sizeof(indices); indices.module_path = modulePath.c_str(); indices.module_address = moduleAddress;
        indices.object_array = fixture.address(32); indices.profile_id = fixture.array.identity.c_str();
        indices.session_id = "owned-object-agent"; indices.layout = mode == "uproperty" ? 1 : 2;
        indices.objects_offset = 0; indices.count_offset = 8; indices.capacity_offset = 12;
        indices.item_stride = 8; indices.item_object_offset = 0;
        indices.maximum_objects = 4; indices.maximum_examined = 4; indices.sample_limit = 4; indices.object_extent = 32;
        const auto& namesProfile = fixture.pool;
        pool = {namesProfile.identity.c_str(), fixture.address(64), *namesProfile.blocks, *namesProfile.header, *namesProfile.string,
            namesProfile.blockBits, namesProfile.stride, namesProfile.maximumBlocks, namesProfile.maximumUnits,
            namesProfile.lengthShift, 0, 0, 4, 0, 0, 8};
    }
    OwnedAgentObjectConfig(const OwnedAgentObjectConfig&) = delete;
    OwnedAgentObjectConfig& operator=(const OwnedAgentObjectConfig&) = delete;
};
