# Data handling

## Actual data access

Memory/ELF utilities inspect native addresses, mappings and module metadata. Input hooks observe host input; ImGui and configuration state can include user-entered text. Logging can include addresses, module names, package identifiers and diagnostic values. Runtime strings remain unchanged in this documentation phase.

Configuration storage uses `<EXTERNAL_STORAGE>/Android/data/<getprogname()>/cache`, with `/sdcard` as the base fallback. Active filenames and marker names derive from `kPROJECT_NAME`. Filenames are not protected by a complete path-containment policy, and updates are not transactional.

Logger paths depend on the compiled logger implementation and project macros; the adjacent AndCommon copy and UE's pinned dependency are not interchangeable. Logcat, per-tag files and buffered timeline files are separate output channels. Write failure, rotation or process termination can lose records; logs are not a complete audit trail.

The export worker writes below `<external-storage>/Android/data/<getprogname()>/files/UEDump3r/<getprogname()>`. It deletes that package directory before generating replacement output. Dumper buffers can contain object/name metadata, executable information and SDK headers/sources; nested keys such as `SDK_A/Basic.hpp` become relative output paths. Nested directory creation and file-write failures are not fully reflected in the success status. Existing exports can therefore be lost or incomplete.

Source: [DumperBridge.cpp](../source/UEProber/DumperBridge.cpp) and the exact pinned AndUEDumper/AndSwapChainHook sources in [dependency notices](../THIRD_PARTY_NOTICES.md).

## Handling expectations and missing controls

There is no verified repository-wide redaction, deletion or retention service. Review and minimize logs/dumps before sharing; remove the specifically identified experiment outputs when they are no longer needed. Identity originals, credentials, customer authorization documents and unapproved third-party data must remain outside Git and public reports. No cleanup command here authorizes deleting unrelated application data.

The review did not dynamically monitor network traffic and does not claim that execution is offline or free of external transmission. Model-assisted work can itself transmit supplied content to the selected platform. Approval for cyber access does not establish permission to disclose private source/data or determine account retention settings. Those require the actual task's data permissions and account configuration.
