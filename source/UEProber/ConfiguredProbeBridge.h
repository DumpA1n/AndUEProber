#pragma once
#include "andueprober/Agent.h"
#include "andueprober/Probe.hpp"
#include "andueprober/Structs.hpp"
#include "andueprober/Classes.hpp"
#include "andueprober/Functions.hpp"
#include "andueprober/Fields.hpp"
#include "andueprober/Properties.hpp"
#include "andueprober/ObjectFlags.hpp"
#include "andueprober/Reflection.hpp"
#include "andueprober/Enums.hpp"
#include "andueprober/PropertyTails.hpp"
#include "andueprober/PropertyValues.hpp"
class UEProber;
struct ConfiguredProbeSetup {
    std::string modulePath, sessionId;
    std::uintptr_t moduleAddress = 0, objectArray = 0, namePool = 0;
    std::uint32_t objectExtent = 0;
    andueprober::ObjectArrayProfile array;
    andueprober::NamePoolProfile pool;
    andueprober::NameLayout names;
    std::vector<andueprober::NameSample> nameSamples, classSamples;
    std::vector<andueprober::PointerSample> outerSamples;
    std::optional<andueprober::ObjectFlagProbeProfile> objectFlags;
    std::vector<andueprober::ObjectFlagSample> objectFlagSamples;
    andueprober::Layout layout = andueprober::Layout::Unknown;
    std::optional<andueprober::StructProbeProfile> structures;
    std::vector<andueprober::StructSample> structSamples;
    std::vector<andueprober::StructFieldSample> fieldSamples;
    std::optional<andueprober::ClassProbeProfile> classes;
    std::vector<andueprober::ClassSample> classFieldSamples;
    std::optional<andueprober::FunctionProbeProfile> functions;
    std::vector<andueprober::FunctionSample> functionSamples;
    std::optional<andueprober::FieldProbeProfile> fieldBases;
    std::vector<andueprober::FieldBaseSample> fieldBaseSamples;
    std::optional<andueprober::PropertyProbeProfile> properties;
    std::vector<andueprober::PropertySample> propertySamples;
    std::optional<andueprober::PropertyTailProfile> propertyTails;
    std::vector<andueprober::PropertyTailSample> propertyTailSamples;
    std::optional<andueprober::BoolPropertyProfile> boolProperties;
    std::vector<andueprober::BoolPropertySample> boolPropertySamples;
    std::optional<andueprober::FieldPathPropertyProfile> fieldPathProperties;
    std::vector<andueprober::FieldPathPropertySample> fieldPathPropertySamples;
    std::optional<andueprober::EnumProbeProfile> enumerations;
    std::vector<andueprober::EnumSample> enumSamples;
    std::optional<andueprober::ReflectionSchema> reflection;
};
andueprober::Status CopyStructProbeOptions(const AUEP_StructProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyObjectFlagProbeOptions(const AUEP_ObjectFlagProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyClassProbeOptions(const AUEP_ClassProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyFunctionProbeOptions(const AUEP_FunctionProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyFieldBaseProbeOptions(const AUEP_FieldBaseProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyPropertyProbeOptions(const AUEP_PropertyProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyPropertyTailProbeOptions(const AUEP_PropertyTailProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyBoolPropertyProbeOptions(const AUEP_BoolPropertyProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyFieldPathPropertyProbeOptions(const AUEP_FieldPathPropertyProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyEnumProbeOptions(const AUEP_EnumProbeOptions*, ConfiguredProbeSetup&);
andueprober::Status CopyFunctionLayoutSchema(const AUEP_LayoutSchema*, ConfiguredProbeSetup&);
andueprober::Status RunConfiguredProbe(const ConfiguredProbeSetup&, UEProber&, const std::string& outputRoot,
    andueprober::Snapshot&, const std::atomic<bool>& cancelled);
