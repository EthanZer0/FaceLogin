# Code signing policy

## Current status

FaceLogin is preparing an application to the [SignPath Foundation](https://signpath.org/).
No approval or Foundation signing certificate has been granted as part of this preparation.
Do not interpret this policy, a GitHub Actions build, or a SHA-256 checksum as evidence that a binary is signed.

If the application is approved, the intended attribution is:
"Free code signing provided by SignPath.io, certificate by SignPath Foundation."
This is a proposed future arrangement, not a description of current releases.

## Responsibilities

- Source authors and maintainers: the FaceLogin contributors, visible in the
  [repository history](https://github.com/EthanZer0/FaceLogin/graphs/contributors).
- Reviewers: maintainers with repository write access review external contributions,
  including changes to build scripts and dependencies.
- Proposed release signing approver and application contact:
  [EthanZer0](https://github.com/EthanZer0). Other approvers must be explicitly designated
  by the team before receiving signing access.
- All members with repository or signing access must use multi-factor authentication.
  The applicant reports GitHub MFA is enabled; SignPath MFA must also be configured
  during onboarding. This document does not independently attest to every contributor's account settings.

## Build and signing scope

The [Windows source build](.github/workflows/windows-build.yml) compiles project sources
on a GitHub-hosted Windows runner, pins action revisions and the vcpkg baseline,
restores the WebView2 SDK by version and SHA-256, and uploads unsigned review artifacts
with a source commit, run URL, dependency notices and binary hashes.
It does not publish a release, request signatures, or include user data or model weights.
Its first successful remote run is still required before claiming a verified CI build.

The proposed signing scope is FaceLogin's own EXE/DLL outputs and, after packaging
eligibility is resolved, the installer and uninstaller. Every production request
requires manual approval and source-origin verification configured in SignPath.
No local certificate private key or signing credential belongs in this repository.

Do not sign upstream DLLs with a Foundation certificate. Preserve valid upstream
signatures; resolve unsigned dependencies with upstream or an approved distribution
strategy. Signing the installer does not sign its embedded DLLs.

## Release gate

Before Foundation signing is enabled:

1. Obtain project acceptance, configure the trusted GitHub build integration and approval roles.
2. Resolve the model licensing and other review items in [THIRD_PARTY.md](THIRD_PARTY.md).
3. Verify product-name/version metadata for each selected binary; current component
   versions differ, and the model probe needs version metadata before signing selection.
4. Publish this policy and the [privacy statement](PRIVACY.md), link them from download pages,
   and update the attribution only after approval.
5. Verify signatures and timestamps, test the final package on Windows with application
   control enabled, then calculate release hashes from the final signed files.

The existing local certificate-based build script is a separate signing route;
it must not be used to bypass Foundation scope restrictions.
