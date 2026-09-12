#pragma once

#include <stdint.h>

#if defined(__GNUC__)
#define AUEP_API __attribute__((visibility("default")))
#else
#define AUEP_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AUEP_Error {
    AUEP_OK = 0, AUEP_INVALID_ARGUMENT = 1, AUEP_BUSY = 2,
    AUEP_NOT_INITIALIZED = 3, AUEP_TARGET_MISMATCH = 4, AUEP_FAILED = 5, AUEP_MISSING_DEPENDENCY = 6, AUEP_UNSUPPORTED = 7
} AUEP_Error;
typedef enum AUEP_State {
    AUEP_CREATED = 0, AUEP_READY = 1, AUEP_RUNNING = 2, AUEP_SUCCEEDED = 3,
    AUEP_CANCELLED = 4, AUEP_STOPPED = 5, AUEP_FAILURE = 6
} AUEP_State;
typedef struct AUEP_Options {
    uint32_t struct_size;
    const char* expected_package;
    const char* output_root;
} AUEP_Options;
typedef struct AUEP_Result {
    AUEP_State state;
    AUEP_Error error;
    char message[512];
} AUEP_Result;

// Describes an owned/authorized object array. Memory remains alive until Stop joins.
// Offsets are relative to TUObjectArray; zero is a valid value. Layout is 1 for
// UProperty or 2 for FField. This operation does not call engine code or emit an SDK.
typedef struct AUEP_ObjectIndexOptions {
    uint32_t struct_size;
    const char* module_path;
    uintptr_t module_address;
    uintptr_t object_array;
    const char* profile_id;
    const char* session_id;
    uint32_t layout;
    uint32_t objects_offset, count_offset, capacity_offset;
    uint32_t item_stride, item_object_offset;
    uint32_t chunk_count_offset, chunk_capacity_offset, elements_per_chunk;
    uint32_t maximum_objects, maximum_examined, sample_limit, object_extent;
} AUEP_ObjectIndexOptions;

// Explicit canonical name layout. Optional display index uses has_display.
typedef struct AUEP_NamePoolOptions {
    const char* profile_id;
    uintptr_t pool;
    uint32_t blocks_offset, header_offset, string_offset;
    uint32_t block_bits, stride, maximum_blocks, maximum_units, length_shift;
    uint32_t narrow_encoding; // 0 = ASCII, 1 = UTF-8.
    uint32_t comparison_offset, number_offset, has_display, display_offset, name_size;
} AUEP_NamePoolOptions;
typedef struct AUEP_ObjectAnchor {
    uintptr_t object, expected_outer;
    const char* identity;
    const char* expected_name;
    const char* expected_class_name;
    const char* outer_identity;
} AUEP_ObjectAnchor;
typedef struct AUEP_ObjectFlagAnchor {
    uintptr_t object;
    const char* identity;
    uint32_t expected_flags;
} AUEP_ObjectFlagAnchor;
typedef struct AUEP_ObjectFlagProbeOptions {
    uint32_t struct_size;
    AUEP_ObjectIndexOptions indices;
    AUEP_NamePoolOptions names;
    const char* metadata_id;
    uint32_t object_anchor_count, flag_anchor_count, flag_extent;
    const AUEP_ObjectAnchor* object_anchors;
    const AUEP_ObjectFlagAnchor* flag_anchors;
} AUEP_ObjectFlagProbeOptions;
typedef struct AUEP_StructAnchor {
    uintptr_t object, expected_super, expected_first_child;
    const char* identity;
    const char* super_identity;
    const char* first_child_identity;
    uint32_t expected_properties_size;
    uint32_t has_child_properties;
    uintptr_t expected_child_properties;
    const char* child_properties_identity;
    uint32_t has_min_alignment, expected_min_alignment;
} AUEP_StructAnchor;
typedef struct AUEP_FieldAnchor {
    uintptr_t object, expected_next;
    const char* identity;
    const char* next_identity;
} AUEP_FieldAnchor;
// Independent metadata must come from declared owned/profile layouts, not a read
// of the field being inferred. All input arrays/strings are copied during start.
// Referenced object/pool/array memory remains caller-owned until Stop joins.
typedef struct AUEP_StructProbeOptions {
    uint32_t struct_size;
    AUEP_ObjectIndexOptions indices;
    AUEP_NamePoolOptions names;
    const char* metadata_id;
    uint32_t struct_extent, field_extent;
    uint32_t object_anchor_count, struct_anchor_count, field_anchor_count;
    const AUEP_ObjectAnchor* object_anchors;
    const AUEP_StructAnchor* struct_anchors;
    const AUEP_FieldAnchor* field_anchors;
} AUEP_StructProbeOptions;

typedef struct AUEP_ClassAnchor {
    uintptr_t object;
    const char* identity;
    uint64_t expected_cast_flags;
    uintptr_t expected_default_object;
    const char* default_object_identity;
} AUEP_ClassAnchor;
// Uses the same copied configuration and caller-owned memory contract as struct probing.
typedef struct AUEP_ClassProbeOptions {
    uint32_t struct_size;
    AUEP_StructProbeOptions structures;
    const char* metadata_id;
    uint32_t class_extent, class_anchor_count;
    const AUEP_ClassAnchor* class_anchors;
} AUEP_ClassProbeOptions;

typedef struct AUEP_FunctionAnchor {
    uintptr_t object;
    const char* identity;
    uint32_t expected_flags;
    uint8_t expected_num_parms;
    uint16_t expected_parms_size, expected_return_offset;
    uintptr_t expected_native_function;
    const char* native_function_identity;
} AUEP_FunctionAnchor;
typedef struct AUEP_FunctionProbeOptions {
    uint32_t struct_size;
    AUEP_ClassProbeOptions classes;
    const char* metadata_id;
    uint32_t function_extent, function_anchor_count;
    const AUEP_FunctionAnchor* function_anchors;
} AUEP_FunctionProbeOptions;

typedef struct AUEP_FieldBaseAnchor {
    uintptr_t object;
    const char* identity;
    const char* expected_name;
    uintptr_t expected_owner;
    const char* owner_identity;
    uint32_t owner_is_uobject;
    uintptr_t expected_next;
    const char* next_identity;
    uintptr_t expected_class;
    const char* class_identity;
    uint32_t expected_flags;
} AUEP_FieldBaseAnchor;
typedef struct AUEP_FieldBaseProbeOptions {
    uint32_t struct_size;
    AUEP_FunctionProbeOptions functions;
    const char* metadata_id;
    uint32_t field_extent, field_anchor_count;
    // 1 = SeparateBoolean. Unknown (0) and tagged (2) representations are unsupported.
    uint32_t owner_representation, owner_pointer_offset, owner_kind_offset, owner_size;
    const AUEP_FieldBaseAnchor* field_anchors;
} AUEP_FieldBaseProbeOptions;
typedef struct AUEP_PropertyAnchor {
    uintptr_t object;
    const char* identity;
    int32_t expected_array_dim, expected_element_size;
    uint64_t expected_property_flags;
    int32_t expected_offset_internal;
    uint32_t containing_value_size;
    const char* containing_value_identity;
} AUEP_PropertyAnchor;
typedef struct AUEP_PropertyProbeOptions {
    uint32_t struct_size;
    AUEP_FieldBaseProbeOptions fields;
    const char* metadata_id;
    // Declared occupied FField prefix, independent of either scan extent.
    uint32_t property_extent, field_base_extent, property_anchor_count;
    const AUEP_PropertyAnchor* property_anchors;
} AUEP_PropertyProbeOptions;

typedef enum AUEP_PropertyTailKind {
    AUEP_TAIL_UNKNOWN = 0, AUEP_TAIL_ENUM = 1, AUEP_TAIL_ARRAY = 2,
    AUEP_TAIL_SET = 3, AUEP_TAIL_MAP = 4, AUEP_TAIL_OBJECT = 5, AUEP_TAIL_BOOL = 6,
    AUEP_TAIL_STRUCT = 7, AUEP_TAIL_BYTE = 8, AUEP_TAIL_CLASS = 9, AUEP_TAIL_INTERFACE = 10
} AUEP_PropertyTailKind;
typedef struct AUEP_PropertyTailAnchor {
    uintptr_t object;
    const char* identity;
    uintptr_t expected_first;
    const char* first_identity;
    uint32_t has_second;
    uintptr_t expected_second;
    const char* second_identity;
} AUEP_PropertyTailAnchor;
typedef struct AUEP_PropertyTailProbeOptions {
    uint32_t struct_size;
    AUEP_PropertyProbeOptions properties;
    const char* metadata_id;
    AUEP_PropertyTailKind kind;
    uint32_t tail_extent, property_base_extent, tail_anchor_count;
    const AUEP_PropertyTailAnchor* tail_anchors;
} AUEP_PropertyTailProbeOptions;

typedef enum AUEP_BoolEncoding {
    AUEP_BOOL_UNKNOWN = 0, AUEP_BOOL_NATIVE_BYTE = 1, AUEP_BOOL_SINGLE_BIT = 2
} AUEP_BoolEncoding;
typedef struct AUEP_BoolPropertyAnchor {
    uintptr_t object;
    const char* identity;
    AUEP_BoolEncoding encoding;
    uint8_t field_size, byte_offset, byte_mask, field_mask;
    uint32_t storage_extent;
    const char* storage_identity;
} AUEP_BoolPropertyAnchor;
typedef struct AUEP_BoolPropertyProbeOptions {
    uint32_t struct_size;
    AUEP_PropertyProbeOptions properties;
    const char* metadata_id;
    uint32_t metadata_extent, property_base_extent, anchor_count;
    const AUEP_BoolPropertyAnchor* anchors;
} AUEP_BoolPropertyProbeOptions;
typedef enum AUEP_FieldPathRepresentation {
    AUEP_FIELD_PATH_UNKNOWN = 0, AUEP_FIELD_PATH_INLINE_FNAME = 1
} AUEP_FieldPathRepresentation;
typedef struct AUEP_FieldPathPropertyAnchor {
    uintptr_t object;
    const char* identity;
    const char* expected_name;
} AUEP_FieldPathPropertyAnchor;
typedef struct AUEP_FieldPathPropertyProbeOptions {
    uint32_t struct_size;
    AUEP_PropertyProbeOptions properties;
    const char* metadata_id;
    AUEP_FieldPathRepresentation representation;
    uint32_t metadata_extent, property_base_extent, anchor_count;
    const AUEP_FieldPathPropertyAnchor* anchors;
} AUEP_FieldPathPropertyProbeOptions;

typedef struct AUEP_EnumValueAnchor {
    const char* expected_name;
    const char* identity;
    int64_t expected_value;
} AUEP_EnumValueAnchor;
typedef struct AUEP_EnumAnchor {
    uintptr_t object;
    const char* identity;
    uint32_t value_count;
    const AUEP_EnumValueAnchor* values;
} AUEP_EnumAnchor;
typedef struct AUEP_EnumProbeOptions {
    uint32_t struct_size;
    AUEP_StructProbeOptions structures;
    const char* metadata_id;
    uint32_t enum_extent, field_base_extent, enum_anchor_count;
    // Every layout field is explicit; offset zero is valid.
    uint32_t array_data_offset, array_count_offset, array_capacity_offset, array_size;
    uint32_t entry_name_offset, entry_value_offset, entry_stride;
    uint32_t maximum_values, maximum_capacity;
    const AUEP_EnumAnchor* enum_anchors;
} AUEP_EnumProbeOptions;

typedef enum AUEP_LayoutScalar {
    AUEP_LAYOUT_UINT8 = 1, AUEP_LAYOUT_UINT16 = 2,
    AUEP_LAYOUT_UINT32 = 3, AUEP_LAYOUT_ADDRESS64 = 4
} AUEP_LayoutScalar;
typedef struct AUEP_LayoutField {
    const char* name;
    const char* offset_source;
    AUEP_LayoutScalar scalar;
} AUEP_LayoutField;
// A single declared UFunction metadata record. Five scalar fields bind to the
// five production UFunction observation keys. No input field supplies an offset.
typedef struct AUEP_LayoutSchema {
    uint32_t struct_size;
    const char* identity;
    uint32_t record_id;
    const char* record_name;
    uint32_t independent_size, independent_alignment;
    const char* metadata_source;
    uint32_t field_count;
    const AUEP_LayoutField* fields;
} AUEP_LayoutSchema;

typedef enum AUEP_CommandKind {
    AUEP_DETECT = 1, AUEP_PROBE_PHASE = 2, AUEP_PROBE_ALL = 3,
    AUEP_SET_OVERRIDE = 4, AUEP_CLEAR_OVERRIDE = 5, AUEP_CLEAR_RESULTS = 6,
    AUEP_EXPORT = 7, AUEP_INSPECT_MEMORY = 8
} AUEP_CommandKind;
typedef struct AUEP_Command {
    uint32_t struct_size;
    AUEP_CommandKind kind;
    uint32_t phase;
    const char* field;
    uint32_t has_value, value;
    uintptr_t address;
    uint32_t size;
    uint64_t generation;
} AUEP_Command;
typedef struct AUEP_CommandResult {
    uint32_t struct_size;
    uint64_t completed, pending, generation;
    uint32_t accepting, running;
    AUEP_Result operation;
} AUEP_CommandResult;

// Strings are copied by initialize. Loading the DSO does not initialize or start the agent.
AUEP_API AUEP_Error AUEP_Initialize(const AUEP_Options* options);
AUEP_API AUEP_Error AUEP_Start(void);
// Probe array indices and publish validated observations through a frozen snapshot.
AUEP_API AUEP_Error AUEP_StartIndexProbe(const AUEP_ObjectIndexOptions* options);
// Observes the four UObject foundation fields followed by independently declared
// uint32 ObjectFlags. Zero and high bits are valid; no mask or engine call is used.
AUEP_API AUEP_Error AUEP_StartObjectFlagProbe(const AUEP_ObjectFlagProbeOptions* options);
// Validates four Core UObject fields, probes configured UStruct/UField metadata,
// and exports frozen observations. Does not discover a game or generate a full SDK.
AUEP_API AUEP_Error AUEP_StartStructProbe(const AUEP_StructProbeOptions* options);
// Runs configured UObject/UStruct phases, then UClass fields from independent
// CastFlags/default-object metadata, and publishes one frozen observation.
AUEP_API AUEP_Error AUEP_StartClassProbe(const AUEP_ClassProbeOptions* options);
// Runs configured phases through UFunction with copied independent metadata.
// Native function pointers are compared as values; this operation never calls them.
AUEP_API AUEP_Error AUEP_StartFunctionProbe(const AUEP_FunctionProbeOptions* options);
// Observes the five configured FField base fields after the preceding phases.
// FProperty and container metadata are outside this operation.
AUEP_API AUEP_Error AUEP_StartFieldBaseProbe(const AUEP_FieldBaseProbeOptions* options);
// Adds four FProperty scalar fields from independent metadata. The containing
// value size is a separate declaration, not the metadata scan extent. Container
// and subclass tails, complete Phase5 and engine calls remain unsupported.
AUEP_API AUEP_Error AUEP_StartPropertyProbe(const AUEP_PropertyProbeOptions* options);
// Observes kind-specific opaque pointers after explicit FProperty metadata.
// Enum/Map/Class use two declared pointer columns; Array/Set/Object/Struct/Byte/
// Interface use one. Unknown
// and Bool kinds are unsupported. Pointer values are never followed or invoked.
AUEP_API AUEP_Error AUEP_StartPropertyTailProbe(const AUEP_PropertyTailProbeOptions* options);
// Copies independent Bool storage declarations and four byte values before start.
// NativeByte is exactly 1/0/1/255; SingleBit has matching one-bit masks.
// Columns scan independently after property_base_extent. Ambiguity is a failure.
AUEP_API AUEP_Error AUEP_StartBoolPropertyProbe(const AUEP_BoolPropertyProbeOptions* options);
// Copies explicit inline-FName metadata. No wrapper layout or class pointer is inferred.
AUEP_API AUEP_Error AUEP_StartFieldPathPropertyProbe(const AUEP_FieldPathPropertyProbeOptions* options);
// Adds UEnum::Names from complete independently declared ordered names and signed
// int64 values after the configured UObject/UField phase. No ProcessEvent is called.
AUEP_API AUEP_Error AUEP_StartEnumProbe(const AUEP_EnumProbeOptions* options);
// Generates a data-layout SDK subset from the final validated function snapshot.
// Declared size/alignment are independent metadata, never taken from read extents.
AUEP_API AUEP_Error AUEP_StartFunctionLayoutProbe(const AUEP_FunctionProbeOptions* options,
    const AUEP_LayoutSchema* schema);
// Starts an idle command owner. Profile detection and probing require explicit commands.
AUEP_API AUEP_Error AUEP_StartInteractive(void);
// Copies the command and field. Admission is bounded to 16 pending commands.
AUEP_API AUEP_Error AUEP_Submit(const AUEP_Command* command, uint64_t* id);
AUEP_API AUEP_Error AUEP_QueryCommands(AUEP_CommandResult* result);
// The caller owns the selected ImGui 1.92.2b context, matching pinned imconfig/layout,
// active frame, renderer and input. All draws use one caller thread. No context is created.
// Automatic providers expose phase, export, candidate-selection and bounded read-only
// inspection commands. Configured sessions render immutable observations without
// command controls, including after Stop.
AUEP_API AUEP_Error AUEP_DrawInspector(void* imgui_context);
AUEP_API AUEP_Error AUEP_Cancel(void);
// Stop closes command admission, requests cancellation and joins the owned worker.
AUEP_API AUEP_Error AUEP_Stop(void);
AUEP_API AUEP_Error AUEP_Query(AUEP_Result* result);

#ifdef __cplusplus
}
#endif
