# Contributing

Changes should state the analysis problem, intended target scope and observable behavior. Technical documentation and ordinary source comments use English. Identifiers, tool directives, third-party licenses and generated data retain their required spelling. Maintainer-facing private review plans may use Chinese.

## Change evidence

- Identify new memory writes, hooks, engine calls, process permissions, signal handlers, background tasks, output paths and changed defaults.
- Distinguish implemented behavior from intended controls and unexecuted test plans.
- Use owned or explicitly authorized fixtures; exclude credentials and private target data from commits and public logs.
- Record dependency source, exact revision or digest, license location and local modifications. A prebuilt binary requires its build provenance or an explicit unresolved provenance entry.
- Test the affected behavior. Record the commit, toolchain, options, target identity and executed, failed or skipped checks. A skipped target is not a pass.

## Documentation and comments

Comments describe current invariants, external constraints, compatibility limits or necessary rationale. Remove obsolete process narration and abandoned commented-out alternatives. Do not hide active capabilities or unresolved defects through terminology changes.

Documentation/comment-only changes preserve executable tokens, directives, string literals, macro continuations and source-line behavior. Logic, dependency, CI and interface changes belong in a separate change with appropriate tests. Review the final diff and affected checks after cleanup.

Private vulnerability reports use [SECURITY.md](SECURITY.md). General source changes use the repository pull-request workflow. The maintenance guidance does not amend existing licenses.
