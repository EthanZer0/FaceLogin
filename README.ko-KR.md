<p align="center">
  <img src="assets/logo.png" alt="FaceLogin 로고" width="200">
</p>

<p align="center">
  <a href="README.md">简体中文</a> · 한국어
</p>

<p align="center">
  Windows Credential Provider 프레임워크를 기반으로 카메라 얼굴 인식 잠금 해제를 제공하는 시스템입니다.<br>
  잠금 화면에 ‘얼굴 로그인’ 타일을 통합하여 얼굴을 바라보는 것만으로 잠금을 해제하며, 로컬 계정과 Microsoft 온라인 계정(MSA)을 모두 지원합니다.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="라이선스"></a>
  <a href="DEVELOPMENT.md"><img src="https://img.shields.io/badge/platform-Windows%2010%2B%20x64-blue" alt="플랫폼"></a>
  <a href="DEVELOPMENT.md"><img src="https://img.shields.io/badge/language-C%2B%2B20%20%7C%20Go-orange" alt="언어"></a>
  <a href="https://github.com/EthanZer0/FaceLogin/releases"><img src="https://img.shields.io/badge/version-1.8.0-green" alt="버전"></a>
</p>

---

## 주요 기능

<div align="center">

| 잠금 화면 얼굴 해제 | 이중 실재 얼굴 확인 | ONNX 인식 |
|:---:|:---:|:---:|
| Windows 기본 잠금 화면 통합<br>추가 조작 불필요 | EAR 눈 깜박임 + facenox MiniFAS<br>사진·영상·마스크 공격 방어 | SCRFD 검출 + InsightFace<br>ONNX 얼굴 인식 |
| **다중 계정 지원** | **안전한 저장** | **실시간 설정 적용** |
| 로컬 SAM + Microsoft 온라인 계정<br>완전 지원, 계정마다 여러 얼굴 등록 가능 | 컴퓨터 범위 DPAPI 암호화<br>파이프 DACL 접근 제어 | 실행 중 인식 설정 변경<br>서비스 재시작 불필요 |

</div>

---

## 시스템 구조

```mermaid
flowchart TB
    subgraph LockScreen["Windows 잠금 화면"]
        LogonUI["LogonUI.exe"]
        CP["FaceLogin<br/>CredentialProvider.dll"]
    end

    subgraph Service["얼굴 인증 서비스"]
        Svc["FaceLoginService.exe"]
    end

    subgraph Enrollment["등록 콘솔"]
        Console["FaceLoginConsole.exe"]
    end

    subgraph Storage["데이터 저장소"]
        direction LR
        UsersDB[("data/<br/>users.dat")] ~~~ Config[("data/<br/>config.json")] ~~~ Models[("models/<br/>ONNX + landmarks")] ~~~ Logs[("log/<br/>로그 파일")]
    end

    LogonUI -->|"COM 인터페이스"| CP
    CP -->|"명명된 파이프"| Svc
    Svc -->|"자격 증명 전달"| CP
    Console -->|"RELOAD_DB / GET_LOGS"| Svc
    Console --> Storage
    Svc --> Storage

    style LockScreen fill:#eff6ff,stroke:#3b82f6
    style Service fill:#fefce8,stroke:#eab308
    style Enrollment fill:#f0fdf4,stroke:#22c55e
    style Storage fill:#fdf2f8,stroke:#ec4899
```

> 모든 프로세스 간 통신은 DACL로 보호된 명명된 파이프 `\\.\pipe\FaceLoginPipe`를 사용합니다.

---

## Star History

<p align="center">
  <a href="https://github.com/EthanZer0/FaceLogin/stargazers">
    <img alt="Star History 차트" src="https://raw.githubusercontent.com/EthanZer0/StarHistory/main/svg/EthanZer0-FaceLogin.svg" width="80%">
  </a>
</p>

> 차트는 별도 프로젝트 [StarHistory](https://github.com/EthanZer0/StarHistory)의 GitHub Actions에서 매일 자동 갱신됩니다. 데이터와 렌더링을 모두 자체 호스팅하며 외부 서비스에 의존하지 않습니다.

---

## 빠른 시작

### 1단계: 설치

[Releases](https://github.com/EthanZer0/FaceLogin/releases)에서 `FaceLoginSetup.exe`를 내려받습니다. 실행 후 설치 폴더를 선택하고 **설치**를 누릅니다.

### 2단계: 얼굴 등록

`FaceLoginConsole.exe`를 관리자 권한으로 실행합니다. 안내에 따라 실재 얼굴 확인을 마치고 암호를 입력한 뒤 **저장하고 등록**을 누릅니다.

### 3단계: 잠금 해제

`Win + L`로 화면을 잠근 뒤 카메라를 바라보면 자동으로 얼굴을 인식하고 잠금을 해제합니다.

### 제거

설치 프로그램을 실행해 **제거** 탭을 선택하거나 `regsvr32 /u FaceLoginCredentialProvider.dll`을 직접 실행합니다.

---

## 시스템 요구 사항

| 요구 사항 | 세부 내용 |
|---|---|
| 운영체제 | Windows 10 21H2+ / Windows 11(x64) |
| 카메라 | USB 또는 내장 카메라, 1280×720 지원 |
| 런타임 | WebView2(Windows 11 기본 포함, Windows 10에서는 자동 설치) |
| 권한 | 관리자 권한(설치 및 등록에 필요) |
| 디스크 공간 | 약 250MB(모델 파일 약 21MB 포함) |

---

## 보안 설계

| 계층 | 보호 조치 |
|---|---|
| 프로세스 통신 | 명명된 파이프 DACL: SYSTEM과 Administrators만 허용, 원격 접근 거부 |
| 자격 증명 저장 | DPAPI `CRYPTPROTECT_LOCAL_MACHINE` 컴퓨터 범위 암호화 |
| 메모리 보호 | 암호 사용 직후 `SecureZeroMemory`로 제거 |
| 실재 얼굴 확인 | EAR 눈 깜박임 + facenox MiniFAS 이중 검증 |
| 얼굴 일치 보안 | 유클리드 거리 기준 + 최상·차상 일치 비율 이중 검증 |
| 빌드 보안 강화 | ASLR, DEP, CFG, 64비트 고엔트로피 주소 무작위화 |

---

## 프로젝트 구조

```
FaceLogin/
├── common/                 # 공용 라이브러리(로그, IPC 프로토콜, DPAPI, 계정 정보, 설정)
├── credential_provider/    # Windows Credential Provider COM DLL
├── face_service/           # 얼굴 인식 Windows 서비스
├── enrollment_app/         # 얼굴 등록 콘솔(WebView2 GUI)
├── installer/              # Go Wails 그래픽 설치 프로그램
├── locales/                # 독립 언어팩 및 번역 관리 안내
├── scripts/                # 보조 스크립트(모델 다운로드, 진단 도구, 언어팩 검사)
└── assets/                 # 아이콘 리소스
```

자세한 기술 문서는 [DEVELOPMENT.md](DEVELOPMENT.md)를 참고하세요.

---

## 소스에서 빌드

> 직접 컴파일할 때만 필요한 내용입니다.

### 사전 요구 사항

- **Visual Studio 2022**(C++ 워크로드 포함)
- **vcpkg** — dlib(이미지 자료 구조: matrix/rectangle/transform), onnxruntime
- **Go 1.25+** + **Wails v2**(설치 프로그램만 해당)
- **CMake 3.20+**

### C++ 구성요소

```powershell
# vcpkg 의존성(인식·검출·랜드마크는 모두 ONNX를 사용하며 dlib은 이미지 자료 구조만 제공)
vcpkg install dlib[core] onnxruntime --triplet x64-windows

# 빌드(FaceLoginConsole.exe는 installer/FaceLoginSetup/resources에 바로 출력됨)
cmake -B build -S . -G "Visual Studio 17 2022" `
    -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

### Go 설치 프로그램

```powershell
cd installer/FaceLoginSetup
# FaceLoginService.exe와 FaceLoginCredentialProvider.dll을 resources에 복사한 뒤 빌드
wails build -clean -platform windows/amd64
```

### 모델 파일

| 파일 | 용도 | 다운로드 |
|---|---|---|
| `2d106det.onnx` | 106개 얼굴 랜드마크 추출 | [InsightFace](https://github.com/deepinsight/insightface) |
| `det_500m.onnx` | SCRFD 얼굴 검출 | [InsightFace](https://github.com/deepinsight/insightface) |
| `w600k_mbf.onnx` | InsightFace 얼굴 인식 | [InsightFace](https://github.com/deepinsight/insightface) |
| `minifas_quantized.onnx` | 수동 동작 없는 위조 방지 | [facenox/face-antispoof-onnx](https://github.com/facenox/face-antispoof-onnx) |

---

## 기여

Issue와 Pull Request를 환영합니다!

- 코드 규칙: C++20, `/W4` 경고 수준
- 제출하기 전에 빌드가 통과하는지 확인하세요.
- 큰 변경은 먼저 Issue를 만들어 논의하세요.

---

## 오픈소스 라이선스

[MIT License](LICENSE) © 2026 美国伐木工&EthanZer0

---

## 면책 조항

이 소프트웨어는 얼굴 인식을 이용해 Windows 로그인을 보조하지만 **암호를 대체할 수는 없습니다**. 얼굴 인식은 편의 기능이며 시스템은 항상 암호 로그인을 예비 수단으로 유지합니다. 보안 요구 수준이 매우 높은 환경에서는 얼굴 인식에만 의존하지 마세요.
