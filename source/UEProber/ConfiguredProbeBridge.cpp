#include "ConfiguredProbeBridge.h"
#include "UEProber.h"
#include "andueprober/Export.hpp"
#if ANDUEPROBER_HAS_DUMPER_ADAPTER
#include "andueprober/DumperAdapter.hpp"
#endif
#if ANDUEPROBER_HAS_PROCESS_MEMORY
#include "andueprober/ProcessMemory.hpp"
#endif
#include <array>
#include <cstring>
#include <limits>

using namespace andueprober;
namespace {
bool textValid(const char* value) { return value && *value && strnlen(value, 1025) <= 1024; }
bool extentValid(std::uintptr_t address, std::uint32_t extent) {
    return address && extent && extent <= 4096 && extent <= std::numeric_limits<std::uintptr_t>::max() - address;
}
Status copyObjectFoundation(const AUEP_ObjectIndexOptions& index, const AUEP_NamePoolOptions& names,
    std::span<const AUEP_ObjectAnchor> anchors, ConfiguredProbeSetup& result) {
    if (index.struct_size != sizeof(index) || anchors.size() < 2 || anchors.size() > 16 ||
        !extentValid(index.object_array, 4096) || !index.module_address || !extentValid(names.pool, 4096) ||
        !index.object_extent || index.object_extent > 4096 || (index.layout != 1 && index.layout != 2) ||
        names.has_display > 1 || names.narrow_encoding > 1)
        return {Error::InvalidArgument, "Object probing requires bounded arrays, extents and canonical name metadata"};
    for (auto offset : {index.objects_offset, index.count_offset, index.capacity_offset, index.item_stride,
        index.item_object_offset, index.chunk_count_offset, index.chunk_capacity_offset, names.blocks_offset,
        names.header_offset, names.string_offset, names.comparison_offset, names.number_offset,
        names.display_offset, names.name_size})
        if (offset > 4096) return {Error::InvalidArgument, "A configured field exceeds its maximum extent"};
    if (!index.item_stride || !index.maximum_objects || index.maximum_objects > 1024 * 1024 ||
        !index.maximum_examined || index.maximum_examined > 4096 || index.sample_limit < 2 || index.sample_limit > 64 ||
        index.elements_per_chunk > 1024 * 1024 || index.item_object_offset > index.item_stride ||
        sizeof(std::uintptr_t) > index.item_stride - index.item_object_offset)
        return {Error::InvalidArgument, "The object-array scan bounds are invalid"};
    if (!textValid(index.module_path) || !textValid(index.profile_id) || !textValid(index.session_id) ||
        !textValid(names.profile_id))
        return {Error::InvalidArgument, "Profile, module and session identities must be bounded strings"};
    for (std::uint32_t i = 0; i < anchors.size(); ++i) {
        const auto& value = anchors[i];
        if (!extentValid(value.object, index.object_extent) || !textValid(value.identity) || !textValid(value.expected_name) ||
            !textValid(value.expected_class_name) || !textValid(value.outer_identity))
            return {Error::InvalidArgument, "Object anchors require bounded addresses, names and relationship identities"};
    }
    ConfiguredProbeSetup copy;
    copy.layout = index.layout == 1 ? Layout::UProperty : Layout::FField;
    copy.modulePath = index.module_path; copy.moduleAddress = index.module_address;
    copy.sessionId = index.session_id; copy.objectArray = index.object_array; copy.objectExtent = index.object_extent;
    copy.array.identity = index.profile_id; copy.array.objects = index.objects_offset; copy.array.count = index.count_offset;
    copy.array.capacity = index.capacity_offset; copy.array.itemStride = index.item_stride; copy.array.itemObject = index.item_object_offset;
    copy.array.chunkCount = index.chunk_count_offset; copy.array.chunkCapacity = index.chunk_capacity_offset;
    copy.array.elementsPerChunk = index.elements_per_chunk; copy.array.maximumObjects = index.maximum_objects;
    copy.array.maximumExamined = index.maximum_examined; copy.array.sampleLimit = index.sample_limit;
    copy.namePool = names.pool; copy.pool.identity = names.profile_id;
    copy.pool.blocks = names.blocks_offset; copy.pool.header = names.header_offset; copy.pool.string = names.string_offset;
    copy.pool.blockBits = names.block_bits; copy.pool.stride = names.stride; copy.pool.maximumBlocks = names.maximum_blocks;
    copy.pool.maximumUnits = names.maximum_units; copy.pool.lengthShift = names.length_shift;
    copy.pool.narrowEncoding = names.narrow_encoding ? NarrowEncoding::Utf8 : NarrowEncoding::Ascii;
    copy.names.comparison = names.comparison_offset; copy.names.number = names.number_offset;
    if (names.has_display) copy.names.display = names.display_offset;
    copy.names.size = names.name_size;
    if (auto status = validateNamePoolProfile(copy.pool); !status) return status;
    if (auto status = validateNameLayout(copy.names); !status) return status;
    for (std::uint32_t i = 0; i < anchors.size(); ++i) {
        const auto& value = anchors[i];
        copy.nameSamples.push_back({value.object, value.expected_name, value.identity});
        copy.classSamples.push_back({value.object, value.expected_class_name, value.identity});
        copy.outerSamples.push_back({value.object, value.expected_outer, value.identity, value.outer_identity});
    }
    result = std::move(copy); return {};
}
}
Status CopyStructProbeOptions(const AUEP_StructProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options)) return {Error::InvalidArgument, "Invalid struct-probe options size"};
    const auto& input = *options; const auto& index = input.indices;
    if (input.object_anchor_count < 2 || input.object_anchor_count > 16 || !input.object_anchors ||
        input.struct_anchor_count < 2 || input.struct_anchor_count > 4 || input.field_anchor_count < 3 || input.field_anchor_count > 64 ||
        !input.struct_anchors || !input.field_anchors || !input.struct_extent || input.struct_extent > 4096 ||
        !input.field_extent || input.field_extent > 4096 || !textValid(input.metadata_id))
        return {Error::InvalidArgument, "Struct probing requires bounded independent metadata"};
    for (std::uint32_t i = 0; i < input.struct_anchor_count; ++i) {
        const auto& value = input.struct_anchors[i];
        if (!extentValid(value.object, input.struct_extent) || !textValid(value.identity) || !textValid(value.super_identity) ||
            !textValid(value.first_child_identity) || value.has_child_properties > 1 || value.has_min_alignment > 1 ||
            value.expected_properties_size > 16 * 1024 * 1024 ||
            (value.has_child_properties && !textValid(value.child_properties_identity)) ||
            (value.has_min_alignment && (!value.expected_min_alignment || value.expected_min_alignment > 4096 ||
                (value.expected_min_alignment & (value.expected_min_alignment - 1)))))
            return {Error::InvalidArgument, "Struct anchors require bounded independent sizes and relationship metadata"};
        if (index.layout == 2 && !value.has_child_properties)
            return {Error::Unsupported, "FField struct probing requires independent child-properties metadata"};
    }
    for (std::uint32_t i = 0; i < input.field_anchor_count; ++i) {
        const auto& value = input.field_anchors[i];
        if (!extentValid(value.object, input.field_extent) || !textValid(value.identity) || !textValid(value.next_identity))
            return {Error::InvalidArgument, "Field anchors require bounded addresses and independent Next identities"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = copyObjectFoundation(index, input.names, {input.object_anchors, input.object_anchor_count}, copy); !status) return status;
    copy.structures = StructProbeProfile{input.metadata_id, {}, 0, copy.layout, input.struct_extent, input.field_extent};
    for (std::uint32_t i = 0; i < input.struct_anchor_count; ++i) {
        const auto& value = input.struct_anchors[i];
        StructSample sample{value.object, value.identity, value.expected_super, value.super_identity,
            value.expected_first_child, value.first_child_identity, value.expected_properties_size, {}, {}, {}};
        if (value.has_child_properties) { sample.expectedChildProperties = value.expected_child_properties; sample.childPropertiesIdentity = value.child_properties_identity; }
        if (value.has_min_alignment) sample.expectedMinAlignment = value.expected_min_alignment;
        copy.structSamples.push_back(std::move(sample));
    }
    for (std::uint32_t i = 0; i < input.field_anchor_count; ++i) {
        const auto& value = input.field_anchors[i];
        copy.fieldSamples.push_back({value.object, value.identity, value.expected_next, value.next_identity});
    }
    result = std::move(copy); return {};
}
Status CopyObjectFlagProbeOptions(const AUEP_ObjectFlagProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options) || !options->object_anchors || !options->flag_anchors ||
        options->object_anchor_count < 2 || options->object_anchor_count > 16 ||
        options->flag_anchor_count < 3 || options->flag_anchor_count > 16 ||
        !options->flag_extent || options->flag_extent > 4096 || !textValid(options->metadata_id))
        return {Error::InvalidArgument, "ObjectFlags probing requires bounded independent metadata"};
    for (std::uint32_t i = 0; i < options->flag_anchor_count; ++i) {
        const auto& value = options->flag_anchors[i];
        if (!extentValid(value.object, options->flag_extent) || !textValid(value.identity))
            return {Error::InvalidArgument, "ObjectFlags anchors require bounded addresses and identities"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = copyObjectFoundation(options->indices, options->names,
        {options->object_anchors, options->object_anchor_count}, copy); !status) return status;
    copy.objectFlags = ObjectFlagProbeProfile{options->metadata_id, {}, 0, copy.layout, options->flag_extent};
    for (std::uint32_t i = 0; i < options->flag_anchor_count; ++i) {
        const auto& value = options->flag_anchors[i];
        copy.objectFlagSamples.push_back({value.object, value.identity, value.expected_flags});
    }
    result = std::move(copy); return {};
}
Status CopyClassProbeOptions(const AUEP_ClassProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options) || !options->class_anchors ||
        options->class_anchor_count < 3 || options->class_anchor_count > 16 ||
        !options->class_extent || options->class_extent > 4096 || !textValid(options->metadata_id))
        return {Error::InvalidArgument, "Class probing requires bounded independent metadata"};
    for (std::uint32_t i = 0; i < options->class_anchor_count; ++i) {
        const auto& sample = options->class_anchors[i];
        if (!extentValid(sample.object, options->class_extent) || !textValid(sample.identity) ||
            !textValid(sample.default_object_identity))
            return {Error::InvalidArgument, "Class anchors require bounded addresses and independent default-object identities"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = CopyStructProbeOptions(&options->structures, copy); !status) return status;
    copy.classes = ClassProbeProfile{options->metadata_id, {}, 0, copy.layout, options->class_extent};
    for (std::uint32_t i = 0; i < options->class_anchor_count; ++i) {
        const auto& value = options->class_anchors[i];
        copy.classFieldSamples.push_back({value.object, value.identity, value.expected_cast_flags,
            value.expected_default_object, value.default_object_identity});
    }
    result = std::move(copy); return {};
}
Status CopyFunctionProbeOptions(const AUEP_FunctionProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options) || !options->function_anchors ||
        options->function_anchor_count < 3 || options->function_anchor_count > 16 ||
        !options->function_extent || options->function_extent > 4096 || !textValid(options->metadata_id))
        return {Error::InvalidArgument, "Function probing requires bounded independent metadata"};
    for (std::uint32_t i = 0; i < options->function_anchor_count; ++i) {
        const auto& sample = options->function_anchors[i];
        if (!extentValid(sample.object, options->function_extent) || !textValid(sample.identity) ||
            !textValid(sample.native_function_identity) || (sample.expected_return_offset != UINT16_MAX &&
                sample.expected_return_offset >= sample.expected_parms_size))
            return {Error::InvalidArgument, "Function anchors require bounded addresses, parameter layouts and native identities"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = CopyClassProbeOptions(&options->classes, copy); !status) return status;
    copy.functions = FunctionProbeProfile{options->metadata_id, {}, 0, copy.layout, options->function_extent};
    for (std::uint32_t i = 0; i < options->function_anchor_count; ++i) {
        const auto& value = options->function_anchors[i];
        copy.functionSamples.push_back({value.object, value.identity, value.expected_flags, value.expected_num_parms,
            value.expected_parms_size, value.expected_return_offset, value.expected_native_function, value.native_function_identity});
    }
    result = std::move(copy); return {};
}
Status CopyFieldBaseProbeOptions(const AUEP_FieldBaseProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options) || !options->field_anchors ||
        options->field_anchor_count < 3 || options->field_anchor_count > 16 ||
        !options->field_extent || options->field_extent > 4096 || !textValid(options->metadata_id) || options->owner_representation > 2)
        return {Error::InvalidArgument, "FField probing requires bounded independent metadata"};
    if (options->owner_representation != 1 || options->functions.classes.structures.indices.layout != 2)
        return {Error::Unsupported, "FField probing requires an explicit SeparateBoolean FField layout"};
    if (options->owner_size < sizeof(std::uintptr_t) + 1 || options->owner_size > 64 ||
        options->owner_pointer_offset > options->owner_size - sizeof(std::uintptr_t) || options->owner_kind_offset >= options->owner_size ||
        (options->owner_kind_offset >= options->owner_pointer_offset && options->owner_kind_offset < options->owner_pointer_offset + sizeof(std::uintptr_t)))
        return {Error::InvalidArgument, "FField owner fields overlap or exceed the declared representation"};
    for (std::uint32_t i = 0; i < options->field_anchor_count; ++i) {
        const auto& sample = options->field_anchors[i];
        if (!extentValid(sample.object, options->field_extent) || !textValid(sample.identity) || !textValid(sample.expected_name) ||
            !textValid(sample.owner_identity) || !textValid(sample.next_identity) || !textValid(sample.class_identity) || sample.owner_is_uobject > 1)
            return {Error::InvalidArgument, "FField anchors require bounded names, addresses and declared relationships"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = CopyFunctionProbeOptions(&options->functions, copy); !status) return status;
    copy.fieldBases = FieldProbeProfile{options->metadata_id, {}, 0, Layout::FField, options->field_extent,
        {FieldOwnerRepresentation::SeparateBoolean, options->owner_pointer_offset, options->owner_kind_offset, options->owner_size}};
    for (std::uint32_t i = 0; i < options->field_anchor_count; ++i) {
        const auto& value = options->field_anchors[i];
        copy.fieldBaseSamples.push_back({value.object, value.identity, value.expected_name, value.expected_owner, value.owner_identity,
            static_cast<bool>(value.owner_is_uobject), value.expected_next, value.next_identity, value.expected_class, value.class_identity, value.expected_flags});
    }
    result = std::move(copy); return {};
}
Status CopyPropertyProbeOptions(const AUEP_PropertyProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options) || !options->property_anchors ||
        options->property_anchor_count < 3 || options->property_anchor_count > 16 ||
        !options->property_extent || options->property_extent > 4096 || !options->field_base_extent ||
        options->field_base_extent >= options->property_extent || !textValid(options->metadata_id))
        return {Error::InvalidArgument, "Property probing requires bounded independent metadata"};
    for (std::uint32_t i = 0; i < options->property_anchor_count; ++i) {
        const auto& sample = options->property_anchors[i];
        if (!extentValid(sample.object, options->property_extent) || !textValid(sample.identity) ||
            !textValid(sample.containing_value_identity) || sample.expected_array_dim <= 0 || sample.expected_element_size <= 0 ||
            sample.expected_offset_internal < 0 || !sample.containing_value_size || sample.containing_value_size > 16 * 1024 * 1024)
            return {Error::InvalidArgument, "Property anchors require bounded positive sizes and nonnegative offsets"};
        const auto bytes = static_cast<std::uint64_t>(sample.expected_array_dim) * static_cast<std::uint64_t>(sample.expected_element_size);
        if (static_cast<std::uint64_t>(sample.expected_offset_internal) + bytes > sample.containing_value_size)
            return {Error::InvalidArgument, "The declared property value exceeds its independent containing size"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = CopyFieldBaseProbeOptions(&options->fields, copy); !status) return status;
    copy.properties = PropertyProbeProfile{options->metadata_id, {}, 0, Layout::FField, options->property_extent};
    copy.properties->fieldBaseExtent = options->field_base_extent;
    copy.properties->fieldBaseProfileIdentity = copy.fieldBases->identity;
    copy.properties->ownerLayout = copy.fieldBases->ownerLayout;
    for (std::uint32_t i = 0; i < options->property_anchor_count; ++i) {
        const auto& value = options->property_anchors[i];
        copy.propertySamples.push_back({value.object, value.identity, value.expected_array_dim, value.expected_element_size,
            value.expected_property_flags, value.expected_offset_internal, value.containing_value_size, value.containing_value_identity});
    }
    result = std::move(copy); return {};
}
Status CopyPropertyTailProbeOptions(const AUEP_PropertyTailProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options))
        return {Error::InvalidArgument, "Invalid property-tail options size"};
    if (options->kind == AUEP_TAIL_UNKNOWN || options->kind == AUEP_TAIL_BOOL)
        return {Error::Unsupported, "Unknown and Bool property-tail representations are unsupported"};
    if (options->kind < AUEP_TAIL_ENUM || options->kind > AUEP_TAIL_INTERFACE || !options->tail_anchors ||
        options->tail_anchor_count < 3 || options->tail_anchor_count > 16 ||
        !options->tail_extent || options->tail_extent > 4096 || !options->property_base_extent ||
        options->property_base_extent >= options->tail_extent || !textValid(options->metadata_id))
        return {Error::InvalidArgument, "Property tails require bounded independent metadata and occupied-prefix declarations"};
    const bool dual = options->kind == AUEP_TAIL_ENUM || options->kind == AUEP_TAIL_MAP || options->kind == AUEP_TAIL_CLASS;
    for (std::uint32_t i = 0; i < options->tail_anchor_count; ++i) {
        const auto& value = options->tail_anchors[i];
        if (!extentValid(value.object, options->tail_extent) || !textValid(value.identity) || !textValid(value.first_identity) ||
            value.has_second > 1 || static_cast<bool>(value.has_second) != dual ||
            (dual ? !textValid(value.second_identity) : value.expected_second || (value.second_identity && *value.second_identity)))
            return {Error::InvalidArgument, "Property-tail anchors require bounded explicit pointer-column identities"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = CopyPropertyProbeOptions(&options->properties, copy); !status) return status;
    copy.propertyTails = PropertyTailProfile{options->metadata_id, *copy.properties, options->tail_extent,
        options->property_base_extent, static_cast<PropertyTailKind>(options->kind)};
    for (std::uint32_t i = 0; i < options->tail_anchor_count; ++i) {
        const auto& value = options->tail_anchors[i];
        PropertyTailSample sample{value.object, value.identity, value.expected_first, value.first_identity, {}, {}};
        if (dual) { sample.expectedSecond = value.expected_second; sample.secondIdentity = value.second_identity; }
        copy.propertyTailSamples.push_back(std::move(sample));
    }
    result = std::move(copy); return {};
}
Status CopyBoolPropertyProbeOptions(const AUEP_BoolPropertyProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options) || !options->anchors || options->anchor_count < 3 || options->anchor_count > 16 ||
        !options->metadata_extent || options->metadata_extent > 4096 || !options->property_base_extent ||
        options->property_base_extent >= options->metadata_extent || !textValid(options->metadata_id))
        return {Error::InvalidArgument, "Bool probing requires bounded independent metadata and occupied-prefix declarations"};
    for (std::uint32_t i = 0; i < options->anchor_count; ++i) {
        const auto& value = options->anchors[i];
        if (value.encoding != AUEP_BOOL_NATIVE_BYTE && value.encoding != AUEP_BOOL_SINGLE_BIT)
            return {Error::Unsupported, "Bool probing requires an explicit NativeByte or SingleBit encoding"};
        if (!extentValid(value.object, options->metadata_extent) || !textValid(value.identity) || !textValid(value.storage_identity) ||
            !value.storage_extent || value.storage_extent > 255 || !value.field_size || value.field_size > value.storage_extent || value.byte_offset >= value.field_size ||
            (value.encoding == AUEP_BOOL_NATIVE_BYTE && (value.field_size != 1 || value.byte_offset != 0 || value.byte_mask != 1 || value.field_mask != 255)) ||
            (value.encoding == AUEP_BOOL_SINGLE_BIT && (!value.byte_mask || (value.byte_mask & (value.byte_mask - 1)) || value.field_mask != value.byte_mask)))
            return {Error::InvalidArgument, "Bool anchors require bounded storage and the exact declared byte encoding"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = CopyPropertyProbeOptions(&options->properties, copy); !status) return status;
    copy.boolProperties = BoolPropertyProfile{options->metadata_id, *copy.properties, options->metadata_extent, options->property_base_extent};
    for (std::uint32_t i = 0; i < options->anchor_count; ++i) {
        const auto& value = options->anchors[i];
        copy.boolPropertySamples.push_back({value.object, value.identity, static_cast<BoolEncoding>(value.encoding),
            value.field_size, value.byte_offset, value.byte_mask, value.field_mask, value.storage_extent, value.storage_identity});
    }
    result = std::move(copy); return {};
}
Status CopyFieldPathPropertyProbeOptions(const AUEP_FieldPathPropertyProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options)) return {Error::InvalidArgument, "Invalid FieldPath options size"};
    if (options->representation != AUEP_FIELD_PATH_INLINE_FNAME)
        return {Error::Unsupported, "FieldPath probing requires an explicitly declared inline FName representation"};
    if (!options->anchors || options->anchor_count < 3 || options->anchor_count > 16 ||
        !options->metadata_extent || options->metadata_extent > 4096 || !options->property_base_extent ||
        options->property_base_extent >= options->metadata_extent || !textValid(options->metadata_id))
        return {Error::InvalidArgument, "FieldPath probing requires bounded independent metadata and occupied-prefix declarations"};
    for (std::uint32_t i = 0; i < options->anchor_count; ++i) {
        const auto& value = options->anchors[i];
        if (!extentValid(value.object, options->metadata_extent) || !textValid(value.identity) || !textValid(value.expected_name))
            return {Error::InvalidArgument, "FieldPath anchors require bounded addresses, identities and complete names"};
    }
    ConfiguredProbeSetup copy;
    if (auto status = CopyPropertyProbeOptions(&options->properties, copy); !status) return status;
    copy.fieldPathProperties = FieldPathPropertyProfile{options->metadata_id, *copy.properties, options->metadata_extent,
        options->property_base_extent, FieldPathRepresentation::InlineFName};
    for (std::uint32_t i = 0; i < options->anchor_count; ++i) {
        const auto& value = options->anchors[i]; copy.fieldPathPropertySamples.push_back({value.object, value.identity, value.expected_name});
    }
    result = std::move(copy); return {};
}
Status CopyEnumProbeOptions(const AUEP_EnumProbeOptions* options, ConfiguredProbeSetup& result) {
    result = {};
    if (!options || options->struct_size != sizeof(*options) || !options->enum_anchors ||
        options->enum_anchor_count < 3 || options->enum_anchor_count > 16 ||
        !options->enum_extent || options->enum_extent > 4096 || !options->field_base_extent ||
        options->field_base_extent > options->enum_extent || !textValid(options->metadata_id) ||
        options->array_size < 16 || options->array_size > 64 ||
        options->array_data_offset > options->array_size - 8 || options->array_count_offset > options->array_size - 4 ||
        options->array_capacity_offset > options->array_size - 4 ||
        options->entry_stride < 8 || options->entry_stride > 128 ||
        options->entry_name_offset >= options->entry_stride || options->entry_value_offset > options->entry_stride - 8 ||
        !options->maximum_values || options->maximum_values > 16384 ||
        options->maximum_capacity < options->maximum_values || options->maximum_capacity > 1048576)
        return {Error::InvalidArgument, "Enum probing requires bounded explicit arrays, entry layouts and independent metadata"};
    std::size_t entries = 0, metadataBytes = 0;
    for (std::uint32_t i = 0; i < options->enum_anchor_count; ++i) {
        const auto& value = options->enum_anchors[i];
        if (!extentValid(value.object, options->enum_extent) || !textValid(value.identity) || !value.values ||
            !value.value_count || value.value_count > options->maximum_values || value.value_count > 16384 - entries)
            return {Error::InvalidArgument, "Enum anchors require bounded addresses, identities and complete declared entries"};
        entries += value.value_count;
        metadataBytes += sizeof(EnumSample) + std::strlen(value.identity);
        for (std::uint32_t j = 0; j < value.value_count; ++j) {
            const auto& entry = value.values[j];
            if (!textValid(entry.expected_name) || !textValid(entry.identity))
                return {Error::InvalidArgument, "Enum values require bounded complete names and independent identities"};
            metadataBytes += sizeof(EnumValueSample) + std::strlen(entry.expected_name) + std::strlen(entry.identity);
            if (metadataBytes > 4 * 1024 * 1024)
                return {Error::InvalidArgument, "Enum declarations exceed the metadata budget"};
        }
    }
    ConfiguredProbeSetup copy;
    if (auto status = CopyStructProbeOptions(&options->structures, copy); !status) return status;
    copy.enumerations = EnumProbeProfile{options->metadata_id, {}, 0, copy.layout,
        options->enum_extent, options->field_base_extent,
        {options->array_data_offset, options->array_count_offset, options->array_capacity_offset, options->array_size},
        {options->entry_name_offset, options->entry_value_offset, options->entry_stride},
        options->maximum_values, options->maximum_capacity};
    for (std::uint32_t i = 0; i < options->enum_anchor_count; ++i) {
        const auto& value = options->enum_anchors[i];
        EnumSample sample{value.object, value.identity, {}};
        for (std::uint32_t j = 0; j < value.value_count; ++j) {
            const auto& entry = value.values[j];
            sample.values.push_back({entry.expected_name, entry.identity, entry.expected_value});
        }
        copy.enumSamples.push_back(std::move(sample));
    }
    result = std::move(copy); return {};
}
Status CopyFunctionLayoutSchema(const AUEP_LayoutSchema* schema, ConfiguredProbeSetup& setup) {
    if (!schema || schema->struct_size != sizeof(*schema) || !schema->fields || schema->field_count != 5 ||
        !textValid(schema->identity) || !schema->record_id || !textValid(schema->record_name) || !textValid(schema->metadata_source) ||
        !schema->independent_size || schema->independent_size > 16 * 1024 * 1024 ||
        !schema->independent_alignment || schema->independent_alignment > 4096 ||
        (schema->independent_alignment & (schema->independent_alignment - 1)) ||
        schema->independent_size % schema->independent_alignment)
        return {Error::InvalidArgument, "A function layout requires bounded independent size, alignment and five named scalar fields"};
    const std::array<std::pair<const char*, AUEP_LayoutScalar>, 5> expected{{
        {"UFunction::FunctionFlags", AUEP_LAYOUT_UINT32}, {"UFunction::NumParms", AUEP_LAYOUT_UINT8},
        {"UFunction::ParmsSize", AUEP_LAYOUT_UINT16}, {"UFunction::ReturnValueOffset", AUEP_LAYOUT_UINT16},
        {"UFunction::Func", AUEP_LAYOUT_ADDRESS64}}};
    std::array<bool, 5> seen{};
    for (std::uint32_t i = 0; i < schema->field_count; ++i) {
        const auto& field = schema->fields[i];
        if (!textValid(field.name) || !textValid(field.offset_source))
            return {Error::InvalidArgument, "Layout fields require bounded names and observation keys"};
        bool matched = false;
        for (std::size_t j = 0; j < expected.size(); ++j) {
            if (std::strcmp(field.offset_source, expected[j].first)) continue;
            if (seen[j] || field.scalar != expected[j].second)
                return {Error::InvalidArgument, "Function layout fields must match the unique observed scalar widths"};
            seen[j] = matched = true; break;
        }
        if (!matched) return {Error::Unsupported, "The function layout subset only accepts the five observed UFunction fields"};
    }
    ReflectionSchema copy; copy.identity = schema->identity;
    ReflectionRecordSpec record; record.id = schema->record_id; record.name = schema->record_name;
    record.size = schema->independent_size; record.alignment = schema->independent_alignment;
    record.metadataSource = schema->metadata_source;
    for (std::uint32_t i = 0; i < schema->field_count; ++i) {
        const auto& field = schema->fields[i];
        const auto scalar = field.scalar == AUEP_LAYOUT_UINT8 ? ReflectionScalar::UInt8 :
            field.scalar == AUEP_LAYOUT_UINT16 ? ReflectionScalar::UInt16 :
            field.scalar == AUEP_LAYOUT_UINT32 ? ReflectionScalar::UInt32 : ReflectionScalar::Address64;
        record.fields.push_back({field.name, {ReflectionTypeKind::Scalar, scalar, 0, 1}, field.offset_source});
    }
    copy.records.push_back(std::move(record)); setup.reflection = std::move(copy); return {};
}
Status RunConfiguredProbe(const ConfiguredProbeSetup& setup, UEProber& prober, const std::string& outputRoot,
    Snapshot& snapshot, const std::atomic<bool>& cancelled) {
#if !ANDUEPROBER_HAS_PROCESS_MEMORY
    (void)setup; (void)prober; (void)outputRoot; (void)snapshot; (void)cancelled;
    return {Error::Unsupported, "The explicit process-memory dependency is unavailable"};
#else
    ReadBudget budget; budget.cancelled = &cancelled; budget.remainingBytes = 64 * 1024 * 1024;
    budget.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    ProcessMemory reader;
    if (auto status = reader.open(setup.modulePath, setup.moduleAddress, &budget); !status) return status;
    snapshot.sessionId = setup.sessionId; snapshot.moduleIdentity = reader.identity(); snapshot.generation = reader.generation();
    snapshot.layout = setup.layout;
    snapshot.layoutIdentity = objectArrayLayoutIdentity(setup.array) + "|" + nameLayoutIdentity(setup.names, setup.pool);
    FieldProbeReport report; report.generation = budget.generation;
    if (auto status = probeObjectArrayIndices(reader, setup.objectArray, setup.array, setup.objectExtent, budget, report.candidates); !status) return status;
    if (auto status = publishFieldProbe(snapshot, "UObject::InternalIndex", report, {}); !status) return status;
    const std::array<std::string, 1> indexDependency{"UObject::InternalIndex"};
    if (auto status = probeNameField(reader, setup.nameSamples, setup.objectExtent, setup.names, setup.namePool, setup.pool, budget, report); !status) return status;
    if (auto status = publishFieldProbe(snapshot, "UObject::NamePrivate", report, indexDependency); !status) return status;
    const std::array<std::string, 2> dependencies{"UObject::InternalIndex", "UObject::NamePrivate"};
    if (auto status = probeClassField(reader, setup.classSamples, setup.objectExtent, snapshot.offsets.at("UObject::NamePrivate").value,
        setup.names, setup.namePool, setup.pool, budget, report); !status) return status;
    if (auto status = publishFieldProbe(snapshot, "UObject::ClassPrivate", report, dependencies); !status) return status;
    if (auto status = probePointerField(reader, setup.outerSamples, setup.objectExtent, setup.array.identity, budget, report); !status) return status;
    if (auto status = publishFieldProbe(snapshot, "UObject::OuterPrivate", report, dependencies); !status) return status;
    if (setup.objectFlags) {
        auto flags = *setup.objectFlags; flags.moduleIdentity = reader.identity(); flags.generation = reader.generation();
        if (auto status = probeObjectFlags(reader, flags, setup.objectFlagSamples, setup.names, setup.namePool, setup.pool, budget, snapshot); !status) return status;
    }
    if (setup.structures) {
        auto profile = *setup.structures; profile.moduleIdentity = reader.identity(); profile.generation = reader.generation();
        if (auto status = prober.RunConfiguredStructPhase(reader, profile, setup.structSamples, setup.fieldSamples, budget, snapshot); !status) return status;
    }
    if (setup.classes) {
        auto classes = *setup.classes; classes.moduleIdentity = reader.identity(); classes.generation = reader.generation();
        if (auto status = prober.RunConfiguredClassPhase(reader, classes, setup.classFieldSamples, budget, snapshot); !status) return status;
    }
    if (setup.functions) {
        auto functions = *setup.functions; functions.moduleIdentity = reader.identity(); functions.generation = reader.generation();
        if (auto status = prober.RunConfiguredFunctionPhase(reader, functions, setup.functionSamples, budget, snapshot); !status) return status;
    }
    if (setup.fieldBases) {
        auto fields = *setup.fieldBases; fields.moduleIdentity = reader.identity(); fields.generation = reader.generation();
        if (auto status = prober.RunConfiguredFieldBasePhase(reader, fields, setup.fieldBaseSamples,
            setup.names, setup.namePool, setup.pool, budget, snapshot); !status) return status;
    }
    if (setup.properties) {
        auto properties = *setup.properties; properties.moduleIdentity = reader.identity(); properties.generation = reader.generation();
        if (auto status = prober.RunConfiguredPropertyPhase(reader, properties, setup.propertySamples,
            setup.names, setup.namePool, setup.pool, budget, snapshot); !status) return status;
    }
    if (setup.propertyTails) {
        auto tails = *setup.propertyTails; tails.property.moduleIdentity = reader.identity(); tails.property.generation = reader.generation();
        if (auto status = prober.RunConfiguredPropertyTailPhase(reader, tails, setup.propertyTailSamples,
            setup.names, setup.namePool, setup.pool, budget, snapshot); !status) return status;
    }
    if (setup.boolProperties) {
        auto profile = *setup.boolProperties; profile.property.moduleIdentity = reader.identity(); profile.property.generation = reader.generation();
        if (auto status = prober.RunConfiguredBoolPropertyPhase(reader, profile, setup.boolPropertySamples,
            setup.names, setup.namePool, setup.pool, budget, snapshot); !status) return status;
    }
    if (setup.fieldPathProperties) {
        auto profile = *setup.fieldPathProperties; profile.property.moduleIdentity = reader.identity(); profile.property.generation = reader.generation();
        if (auto status = prober.RunConfiguredFieldPathPropertyPhase(reader, profile, setup.fieldPathPropertySamples,
            setup.names, setup.namePool, setup.pool, budget, snapshot); !status) return status;
    }
    if (setup.enumerations) {
        auto enums = *setup.enumerations; enums.moduleIdentity = reader.identity(); enums.generation = reader.generation();
        if (auto status = prober.RunConfiguredEnumPhase(reader, enums, setup.enumSamples,
            setup.names, setup.namePool, setup.pool, budget, snapshot); !status) return status;
    }
    const auto frozen = snapshot;
    std::string observations;
    for (const auto& [name, value] : frozen.offsets)
        if (value.value) observations += name + " = " + std::to_string(*value.value) + "\n";
    ExportOptions publication; publication.root = outputRoot; publication.cancelled = &cancelled;
    publication.deadline = budget.deadline;
    publication.dependencyRevisions = {{"AndSwapChainHook.Memory", ProcessMemory::providerIdentity()}};
    const auto filename = setup.boolProperties ? "bool-property-observations.txt" : setup.fieldPathProperties ? "field-path-property-observations.txt" : setup.propertyTails ? "property-tail-observations.txt" : setup.enumerations ? "enum-observations.txt" : setup.objectFlags ? "object-flag-observations.txt" : setup.properties ? "property-observations.txt" : setup.fieldBases ? "field-base-observations.txt" : setup.functions ? "function-observations.txt" :
        setup.classes ? "class-observations.txt" : "struct-observations.txt";
    std::vector<ExportFile> files{{filename, observations}};
    if (setup.reflection) {
#if !ANDUEPROBER_HAS_DUMPER_ADAPTER
        return {Error::Unsupported, "The frozen reflection dumper dependency is unavailable"};
#else
        ReflectionLimits reflectionLimits; reflectionLimits.cancelled = &cancelled;
        reflectionLimits.deadline = budget.deadline;
        auto reflection = freezeReflection(frozen, *setup.reflection, reflectionLimits);
        if (!reflection.status) return reflection.status;
        DumperOptions formatting; formatting.cancelled = &cancelled; formatting.deadline = budget.deadline;
        auto header = buildDumperHeader(*reflection.snapshot, formatting);
        if (!header.status) return header.status;
        publication.dependencyRevisions["AndUEDumper.DetachedEmitter"] = dumperDependencyIdentity();
        files.push_back({"SDK/FunctionLayout.hpp", std::move(header.header)});
        const auto& record = reflection.snapshot->records().front();
        std::string declaration = reflection.snapshot->schemaIdentity() + "\n" + record.metadataSource + "\n" +
            "record = " + record.name + "\nindependent-size = " + std::to_string(record.size) +
            "\nindependent-alignment = " + std::to_string(record.alignment) + "\n";
        for (const auto& field : record.fields)
            declaration += field.name + " = " + field.offsetSource + ";offset=" + std::to_string(field.offset) +
                ";version=" + std::to_string(field.offsetVersion) + "\n";
        files.push_back({"layout-schema.txt", std::move(declaration)});
#endif
    }
    return publishExport(frozen, files, publication).status;
#endif
}
