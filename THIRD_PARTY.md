# Third-party dependency and model review

Last reviewed: 2026-10-01. This is a licensing inventory and outstanding-review list,
not a legal opinion or a claim that all packaged files have an OSI-approved license.
FaceLogin's MIT license applies to its own source, not automatically to dependencies,
model weights or training data.

## Software dependencies

| Component | Role | License / review source |
|---|---|---|
| dlib | C++ image utilities; linked into our applications | [Boost Software License](https://github.com/davisking/dlib/blob/master/dlib/LICENSE.txt) |
| ONNX Runtime | Inference runtime and provider DLL | [MIT, plus dependency notices](https://github.com/microsoft/onnxruntime/blob/main/LICENSE) |
| protobuf, RE2 | ONNX Runtime dependencies | [protobuf BSD terms](https://github.com/protocolbuffers/protobuf/blob/main/LICENSE), [RE2 BSD terms](https://github.com/google/re2/blob/main/LICENSE) |
| Abseil | ONNX Runtime dependency | [Apache-2.0](https://github.com/abseil/abseil-cpp/blob/master/LICENSE) |
| OpenBLAS, reference LAPACK | Numerical runtime | [OpenBLAS BSD terms](https://github.com/OpenMathLib/OpenBLAS/blob/develop/LICENSE), [LAPACK BSD terms](https://github.com/Reference-LAPACK/lapack/blob/master/LICENSE) |
| GCC/MinGW runtime DLLs | Fortran/numerical-library runtime | [GCC runtime exception](https://www.gnu.org/licenses/gcc-exception-3.1.html), associated GPL/LGPL and MinGW notices; inspect the actual toolchain package, do not classify all DLLs as MIT |
| WebView2 SDK static loader | Linked loader for Console and diagnostics | Microsoft redistribution license in [LICENSE.txt](enrollment_app/webview2/LICENSE.txt) and [NOTICE.txt](enrollment_app/webview2/NOTICE.txt); do not classify the whole SDK/runtime as MIT or assume a Foundation system-library exception without review |
| Windows/WebView2 runtime | Host-system components | Separate Microsoft terms; the SDK loader and separately installed runtime must be distinguished |
| Go / Wails / Vue / Vite and transitive modules | Installer, uninstaller and frontend | Inspect `go.mod`, `go.sum` and `frontend/package-lock.json`; Wails/Vue/Vite use MIT, but this does not establish the licenses of every transitive module |

The Windows CI artifact copies vcpkg copyright texts and package SPDX records,
FaceLogin and WebView2 notices, and available Go/npm module notices. It exports
`go-modules.json` and `npm-packages.json` with versions and notice availability.
These are build-specific evidence, not a complete installer SBOM. Modules with
no exported notice, npm license declarations, GCC corresponding-source obligations,
and notices actually delivered by the full installer still need review before
a Foundation-signed release.

## Pretrained model weights

| Distributed filename | Recorded source | Status |
|---|---|---|
| `det_500m.onnx` | InsightFace model zoo | Upstream pretrained-model restriction: non-commercial research only; not covered by the repository's MIT code license |
| `w600k_mbf.onnx` | InsightFace model zoo | Same restriction; explicit eligibility/redistribution clarification required |
| `2d106det.onnx` | InsightFace model zoo | Same restriction; precise asset provenance must also be recorded |
| `minifas_quantized.onnx` | [facenox/face-antispoof-onnx](https://github.com/facenox/face-antispoof-onnx) | Repository advertises Apache-2.0; confirm the exact exported weights, source revision and dataset terms before claiming complete coverage |
| `head_pose_mobilenetv2.onnx` | [yakhyo release](https://github.com/yakhyo/head-pose-estimation/releases/tag/weights) | MIT repository notice and asset hash recorded in [the bundled notice](installer/FaceLoginSetup/resources/models/head_pose_mobilenetv2.LICENSE.txt); dataset/weight terms remain independently reviewable |

InsightFace explicitly distinguishes its MIT software from the pretrained models.
See the [official model-zoo terms](https://github.com/deepinsight/insightface/blob/master/model_zoo/README.md).
Being downloadable, free of charge or used non-commercially does not make model
weights OSI-licensed. The Foundation's [eligibility conditions](https://signpath.org/terms.html)
require open-source components and must be discussed with the Foundation honestly.

## Signing boundaries and release blockers

- Foundation certificates may not be used to re-sign upstream third-party DLLs.
  Preserve upstream signatures and resolve unsigned DLL loading separately.
- Do not state that the current complete installation package is fully eligible.
  Obtain model licensing clarification or choose a separately approved model/package
  strategy; do not silently replace recognition models or change existing templates.
- WebView2 loader classification and all bundled dependency notices need confirmation.
- Current CI review artifacts intentionally omit all model files and the full installer.
  They are not a functional, deployable FaceLogin distribution.
- The local installation resources have not been removed or relabeled by this review.
