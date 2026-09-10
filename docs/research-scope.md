# Research scope

AndUEProber is intended for Unreal Engine reflection and runtime analysis on owned or explicitly authorized systems. This document defines intended research and maintenance scope. The source does not enforce target authorization, and repository visibility or a package profile is not evidence of permission.

## Capability inventory

Source anchors: [Library.cpp](../source/Library.cpp), [UEProber.cpp](../source/UEProber/UEProber.cpp), [DumperBridge.cpp](../source/UEProber/DumperBridge.cpp).

| Capability | Implementation or entry | Side effects and limits |
|---|---|---|
| Reflection probing | Phase1–Phase6 methods | Reads native objects, interprets layouts and invokes profile name-resolution code; results are heuristic |
| Engine invocation | ProcessEvent paths | Calls host functions; game-thread and executable-range validation are incomplete |
| SDK export | DumperBridge and pinned AndUEDumper | Reads object/name metadata and writes files; deletes the existing package export directory first |
| Automatic tasks | Library.cpp worker | Automatic dump after eight seconds; package-prefix BSS mprotect loop every ten milliseconds |
| Graphics/input | Pinned AndSwapChainHook | Hooks host presentation and input; does not inherit changes from the adjacent checkout |
| Startup | JNI_OnLoad key 1337 | Starts a detached worker; scan failure can deliberately crash; key is not authentication |

“Source present” means code is in the checkout. “Built” means a specific target compiled and linked. “Executed” requires a recorded runtime test with a target identity. These states are not interchangeable.

## Research boundary

A test record should identify the owner or authorization basis, exact application/build and device, permitted operations, output directory, data disclosure limits and stop condition. Third-party package names alone do not satisfy that record. The current inventory does not assert permission for any third-party application.

Maintained research examples should use owned fixtures. Unauthorized access, credential theft, data exfiltration, destructive activity, and bypassing protective controls on systems outside an authorized assessment are outside the intended maintenance scope. Existing dual-use functionality is disclosed above; this statement does not remove or disable it.

## Current gaps

[README](../README.md) lists unresolved correctness and lifecycle defects. Runtime target restrictions, inert loading, safe shutdown, complete export controls and a versioned owned-app demonstration are development goals where absent, not current guarantees. The repository has no claimed CVP/TAC approval or software security certificate.

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

The profiles contain target-specific object/name access and ProcessEvent metadata. The identifiers are not an endorsed target list. ProcessEvent resolution for PUBGMHD remains incomplete; versioned test evidence is not supplied for the profile capability claims.

`DumperBridge::GetExProfiles` constructs 33 profiles: the seven first-party definitions above plus 26 from the pinned AndUEDumper snapshot. Upstream profile classes are `PESProfile`, `DislyteProfile`, `MortalKombatProfile`, `FarlightProfile`, `TorchlightProfile`, `BlackCloverProfile`, `WutheringWavesProfile`, `RealBoxing2Profile`, `OdinValhallaProfile`, `Injustice2Profile`, `RooftopParkourProfile`, `BabyYellowProfile`, `TowerFantasyProfile`, `BladeSoulProfile`, `Lineage2Profile`, `Case2Profile`, `CenturyProfile`, `KingArthurProfile`, `NightCrowsProfile`, `HelloNeighborProfile`, `HelloNeighborNDProfile`, `SFG2Profile`, `ArkUltimateProfile`, `AuroriaProfile`, `LineageWProfile`, `RLSideswipeProfile`. Their AppID lists are defined by the pinned dependency, not by this README. No target authorization or runtime compatibility is asserted for those profiles.
