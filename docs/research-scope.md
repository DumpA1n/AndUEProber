# Research scope

AndUEProber is intended for Unreal Engine reflection and runtime analysis on owned or explicitly authorized systems. This document defines intended research and maintenance scope. The source does not enforce target authorization, and repository visibility or a package profile is not evidence of permission.

## Capability inventory

Source anchors: [Library.cpp](../source/Library.cpp), [UEProber.cpp](../source/UEProber/UEProber.cpp), [DumperBridge.cpp](../source/UEProber/DumperBridge.cpp).

| Capability | Implementation or entry | Side effects and limits |
|---|---|---|
| Reflection probing | Core/Probe configured foundation, flags, structure, class, function, field/property/opaque-subclass-pointer/declared-Bool/inline-FieldPath and enum observations | Reads and deep dependency closures are bounded; automatic metadata collection, custom names and complete container/subclass schemas remain unavailable |
| Engine invocation | Executor, signature and allocator admission | No enabled engine invocation or trial calls without the required verified contracts |
| SDK export | Core transactional exporter and public DumperAdapter | Frozen declared data-layout headers and observations preserve prior outputs; complete UE property/container collection and full reflected SDK export remain unavailable |
| Agent operations | AUEP_Initialize/Start/StartIndexProbe/StartObjectFlagProbe/StartStructProbe/StartClassProbe/StartFunctionProbe/StartFieldBaseProbe/StartPropertyProbe/StartPropertyTailProbe/StartBoolPropertyProbe/StartFieldPathPropertyProbe/StartEnumProbe/StartFunctionLayoutProbe/StartInteractive/Submit/Cancel/Stop/Query | Explicit exact-package selection and a single joinable execution owner; standard paths do not enter the upstream live dumper |
| Inspector UI | AUEP_DrawInspector, restricted command view and snapshot-only configured view | Caller-owned compatible ImGui context/frame/thread; no process reads on the drawing thread |
| Graphics/input providers | Excluded from standard agent | Caller owns renderer and input; no automatic hook or provider installation |
| Loading | JNI_OnLoad | Returns JNI version; reserved injector keys do not start operations |

“Source present” means code is in the checkout. “Built” means a specific target compiled and linked. “Executed” requires a recorded runtime test with a target identity. These states are not interchangeable.

## Research boundary

A test record should identify the owner or authorization basis, exact application/build and device, permitted operations, output directory, data disclosure limits and stop condition. Third-party package names alone do not satisfy that record. The current inventory does not assert permission for any third-party application.

Maintained research examples should use owned fixtures. Unauthorized access, credential theft, data exfiltration, destructive activity, and bypassing protective controls on systems outside an authorized assessment are outside the intended maintenance scope. Existing dual-use functionality is disclosed above; this statement does not remove or disable it.

## Current gaps

[README](../README.md) lists unresolved correctness and lifecycle defects. Exact-package selection, inert JNI loading, owned session shutdown and transactional export controls are implemented. Full Android lifecycle verification, complete UE result provenance and a versioned owned UE application demonstration remain incomplete. The repository has no claimed CVP/TAC approval or software security certificate.

## Profile identifiers

The following identifiers are present in first-party profiles. The table records source coverage only; authorization, application version and current runtime compatibility are not verified.

| Profile | Package identifier |
|---|---|
| DeltaForce.hpp | `com.proxima.dfm`, `com.garena.game.df`, `com.tencent.tmgp.dfm` |
| NiZhan.hpp | `com.tencent.tmgp.nz` |
| RocoKingdom.hpp | `com.tencent.nrc` |
| ArenaBreakout.hpp | `com.tencent.mf.uam` |
| Valorant.hpp | `com.tencent.tmgp.codev` |
| PUBG.hpp | `com.tencent.ig`, `com.rekoo.pubgm`, `com.pubg.imobile`, `com.pubg.krmobile`, `com.vng.pubgmobile` |
| PUBGMHD.hpp | `com.tencent.tmgp.pubgmhd` |

The profiles contain target-specific data layouts and declared identifiers. First-party discovery uses the bounded provider; custom names and ProcessEvent discovery are unsupported. The identifiers are not an endorsed target list. ProcessEvent resolution for PUBGMHD remains incomplete; versioned test evidence is not supplied for the profile capability claims.

`DumperBridge::GetExProfiles` constructs 33 profiles: the seven first-party definitions above plus 26 from the pinned AndUEDumper snapshot. Upstream profile classes are `PESProfile`, `DislyteProfile`, `MortalKombatProfile`, `FarlightProfile`, `TorchlightProfile`, `BlackCloverProfile`, `WutheringWavesProfile`, `RealBoxing2Profile`, `OdinValhallaProfile`, `Injustice2Profile`, `RooftopParkourProfile`, `BabyYellowProfile`, `TowerFantasyProfile`, `BladeSoulProfile`, `Lineage2Profile`, `Case2Profile`, `CenturyProfile`, `KingArthurProfile`, `NightCrowsProfile`, `HelloNeighborProfile`, `HelloNeighborNDProfile`, `SFG2Profile`, `ArkUltimateProfile`, `AuroriaProfile`, `LineageWProfile`, `RLSideswipeProfile`. Their AppID lists are defined by the pinned dependency, not by this README. No target authorization or runtime compatibility is asserted for those profiles.
