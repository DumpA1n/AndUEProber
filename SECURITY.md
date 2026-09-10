# Security reporting

## Private reports

The maintainer confirms GitHub Private Vulnerability Reporting is enabled. Reports can be submitted through [AndUEProber private reporting](https://github.com/DumpA1n/AndUEProber/security/advisories/new). Access to private repositories also requires repository access. This channel has not been independently tested by sending a report.

Do not post credentials, private target data or unredacted memory dumps in public issues. A report should include the affected commit, build options, platform/ABI, trigger, observed impact and a minimal owned-fixture reproduction. Large or sensitive evidence can be described first and exchanged through the private report after agreement.

## Support status

The development branch is the maintenance focus. There is no declared supported-release matrix, response SLA or dedicated security team. Known correctness and runtime limitations are listed in [README](README.md) and the [research scope](docs/research-scope.md). Compilation is not a security guarantee.

## Handling workflow

The proposed maintainer workflow is private triage, reproduction within an agreed target scope, impact assessment, a tested fix where feasible, and coordinated advisory/publication. Publication timing and affected-version statements require confirmation for the specific report. This document does not claim that any report has completed that workflow.
