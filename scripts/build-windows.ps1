[CmdletBinding()]
param(
    [string]$BuildDir = "",
    [string]$CacheRoot = "",
    [switch]$InstallVcpkg
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
if ([string]::IsNullOrWhiteSpace($CacheRoot)) {
    $CacheRoot = Join-Path (Split-Path -Parent $RepoRoot) "FaceLoginCache"
}
$CacheRoot = [IO.Path]::GetFullPath($CacheRoot)

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $CacheRoot "build-msvc-vs-script"
}
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

New-Item -ItemType Directory -Path $CacheRoot -Force | Out-Null

function Find-VisualStudioInstallation {
    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    )

    foreach ($vswhere in $vswhereCandidates) {
        if (-not (Test-Path -LiteralPath $vswhere)) {
            continue
        }

        $installationPath = (& $vswhere -latest -products * -property installationPath | Select-Object -First 1).Trim()
        if (-not [string]::IsNullOrWhiteSpace($installationPath)) {
            return $installationPath
        }
    }

    $fallbackRoots = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\2022")
    ) | Where-Object { Test-Path -LiteralPath $_ }

    foreach ($root in $fallbackRoots) {
        $cmake = Get-ChildItem -LiteralPath $root -Filter "cmake.exe" -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "Common7[\\/]IDE[\\/]CommonExtensions[\\/]Microsoft[\\/]CMake" } |
            Select-Object -First 1
        if ($null -ne $cmake) {
            return ($cmake.FullName -replace "[\\/]Common7[\\/]IDE[\\/]CommonExtensions[\\/]Microsoft[\\/]CMake[\\/]CMake[\\/]bin[\\/]cmake\.exe$", "")
        }
    }

    throw "未找到带 C++ 工具链的 Visual Studio 安装。请安装 Visual Studio 2022 Build Tools（含 MSVC 和 Windows SDK）。"
}

function Resolve-VcpkgRoot {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        $candidates += $env:VCPKG_ROOT
    }
    $candidates += @(
        (Join-Path $CacheRoot ".vcpkg-registry"),
        (Join-Path $RepoRoot ".vcpkg-registry"),
        (Join-Path $RepoRoot "vcpkg"),
        "C:\vcpkg"
    )

    foreach ($candidate in ($candidates | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)) {
        $toolchain = Join-Path $candidate "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path -LiteralPath $toolchain) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }

    throw "未找到 vcpkg。请设置 VCPKG_ROOT，或将 vcpkg 放到 FaceLoginCache\.vcpkg-registry。"
}

$vsInstall = Find-VisualStudioInstallation
$CMake = Join-Path $vsInstall "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if (-not (Test-Path -LiteralPath $CMake)) {
    throw "Visual Studio 安装中缺少 CMake：$CMake"
}

$VcpkgRoot = Resolve-VcpkgRoot
$Toolchain = Join-Path $VcpkgRoot "scripts\buildsystems\vcpkg.cmake"
$VcpkgInstalledDir = Join-Path $CacheRoot "vcpkg_installed"
$VcpkgDownloads = Join-Path $CacheRoot "vcpkg-downloads"
$VcpkgBinaryCache = Join-Path $CacheRoot "vcpkg-binary-cache"

New-Item -ItemType Directory -Path $VcpkgDownloads,$VcpkgBinaryCache -Force | Out-Null
$env:VCPKG_ROOT = $VcpkgRoot
$env:VCPKG_DOWNLOADS = $VcpkgDownloads
$env:VCPKG_DEFAULT_BINARY_CACHE = $VcpkgBinaryCache

$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path -LiteralPath $cacheFile) {
    $cacheText = Get-Content -LiteralPath $cacheFile -Raw
    $sourceMatch = [regex]::Match($cacheText, "(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$")
    $generatorMatch = [regex]::Match($cacheText, "(?m)^CMAKE_GENERATOR:INTERNAL=(.+)$")
    $binaryMatch = [regex]::Match($cacheText, "(?m)^(?:FaceLogin_BINARY_DIR:STATIC|CMAKE_CACHEFILE_DIR:INTERNAL)=(.+)$")
    $toolchainMatch = [regex]::Match($cacheText, "(?m)^CMAKE_TOOLCHAIN_FILE:[^=]+=(.+)$")
    $configuredSource = if ($sourceMatch.Success) { $sourceMatch.Groups[1].Value.Trim().Replace("/", "\").TrimEnd("\") } else { "" }
    $configuredGenerator = if ($generatorMatch.Success) { $generatorMatch.Groups[1].Value.Trim() } else { "" }
    $configuredBinary = if ($binaryMatch.Success) { $binaryMatch.Groups[1].Value.Trim().Replace("/", "\").TrimEnd("\") } else { "" }
    $configuredToolchain = if ($toolchainMatch.Success) { $toolchainMatch.Groups[1].Value.Trim().Replace("/", "\").TrimEnd("\") } else { "" }
    $normalizedRepo = $RepoRoot.Replace("/", "\").TrimEnd("\")
    $normalizedBuild = $BuildDir.Replace("/", "\").TrimEnd("\")
    $normalizedToolchain = $Toolchain.Replace("/", "\").TrimEnd("\")

    if ($configuredSource -and -not $configuredSource.Equals($normalizedRepo, [StringComparison]::OrdinalIgnoreCase)) {
        throw "构建目录已绑定到其他源码目录：$BuildDir。请使用新的 -BuildDir，或清理该目录后重试。"
    }
    if ($configuredGenerator -and $configuredGenerator -ne "Visual Studio 17 2022") {
        throw "构建目录使用了不兼容的生成器 '$configuredGenerator'。请使用新的 -BuildDir，或清理该目录后重试。"
    }
    if ($configuredBinary -and -not $configuredBinary.Equals($normalizedBuild, [StringComparison]::OrdinalIgnoreCase)) {
        throw "构建目录记录的二进制路径已失效：$configuredBinary。请使用新的 -BuildDir，或清理该目录后重试。"
    }
    if ($configuredToolchain -and -not $configuredToolchain.Equals($normalizedToolchain, [StringComparison]::OrdinalIgnoreCase)) {
        throw "构建目录记录的 vcpkg 工具链路径已失效：$configuredToolchain。请使用新的 -BuildDir，或清理该目录后重试。"
    }
}

Write-Host "使用 Visual Studio CMake: $CMake"
Write-Host "源码目录: $RepoRoot"
Write-Host "构建目录: $BuildDir"
Write-Host "vcpkg 根目录: $VcpkgRoot"
Write-Host "vcpkg 安装目录: $VcpkgInstalledDir"

$configureArgs = @(
    "-S", $RepoRoot,
    "-B", $BuildDir,
    "-G", "Visual Studio 17 2022",
    "-A", "x64",
    "-DCMAKE_TOOLCHAIN_FILE=$Toolchain",
    "-DVCPKG_INSTALLED_DIR=$VcpkgInstalledDir"
)

if (-not $InstallVcpkg) {
    # 复用外部缓存中已经安装的包，避免每次构建都访问网络或更新 vcpkg 注册表。
    $configureArgs += "-DVCPKG_MANIFEST_MODE=OFF"
    $configureArgs += "-DVCPKG_MANIFEST_INSTALL=OFF"
}

& $CMake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "CMake 配置失败，退出码：$LASTEXITCODE"
}

& $CMake --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    throw "CMake 编译失败，退出码：$LASTEXITCODE"
}

Write-Host "Windows C++ 构建完成。"
