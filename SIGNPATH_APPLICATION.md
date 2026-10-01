# SignPath application preparation

Status: draft, not submitted and not approved.

## Public project information

- Project: FaceLogin
- Repository/homepage: https://github.com/EthanZer0/FaceLogin
- Public releases: https://github.com/EthanZer0/FaceLogin/releases
- Maintainer type: independent community project, no formal organization claimed
- Source license: MIT; dependency and model licenses are separate
- Intended build system: GitHub Actions, see `.github/workflows/windows-build.yml`
- Code signing policy: `CODE_SIGNING.md`
- Privacy statement: `PRIVACY.md`
- Dependency review: `THIRD_PARTY.md`

Applicant contact details belong in the private application, not this document.

## Description for the application

FaceLogin is an MIT-licensed Windows application that enables face-based sign-in
and workstation unlocking using ordinary webcams. It integrates with Windows
through a Credential Provider and a background service, and includes a desktop
console for enrollment and configuration. Optional liveness checks help mitigate
photo spoofing. The project is publicly developed and maintained on GitHub.

## Build verification

The workflow checks out its triggering source revision, pins third-party action
revisions and the manifest's vcpkg baseline, verifies the WebView2 SDK package hash,
and builds the native components and standalone Wails uninstaller. `npm ci` uses
the checked-in lockfile. It uploads unsigned, model-free review components with
dependency notices and a build receipt. It does not request signatures or publish releases.

The workflow has not yet been verified on GitHub as part of this preparation.
Before submission, publish the workflow and policy pages and obtain a successful
run URL. A local parse/build check alone does not prove a successful hosted run.

## Disclosure to the reviewer

The existing full installer includes InsightFace pretrained weights, whose
upstream terms restrict them to non-commercial research. We do not claim those
weights are MIT/OSI-licensed. The CI review artifact omits all model weights and
the full installer while eligibility and redistribution are clarified.
We also request guidance on the Microsoft WebView2 static loader's eligibility.
Our proposed signing scope is our own built components only; we understand that
Foundation certificates cannot be used to re-sign upstream runtime DLLs.

## Before the applicant submits

1. Confirm the Foundation accepts the intended distribution and component scope;
   resolve the model/loader issues rather than checking a blanket compliance statement.
2. Publish these pages and record a successful GitHub Actions run.
3. Confirm the proposed approver, review roles, and MFA for all members with relevant access.
4. Complete module notices and selected-binary product/version metadata before production signing.
5. Have the applicant confirm the Code of Conduct and personal-data processing terms.
6. Submit once, record the response, and complete service onboarding if accepted.

Only after acceptance should download pages say signing is provided by SignPath
Foundation. The existing certificate-store signing script is not a SignPath integration.
