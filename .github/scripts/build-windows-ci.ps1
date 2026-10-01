[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CacheRoot,
    [Parameter(Mandatory)][string]$OutputDir,
    # Local checks may reuse installed packages; the hosted workflow never sets this.
    [switch]$UseInstalledDependencies
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$CacheRoot = [IO.Path]::GetFullPath($CacheRoot)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ((Test-Path -LiteralPath $OutputDir) -and
    (Get-ChildItem -LiteralPath $OutputDir -Force | Select-Object -First 1)) {
    throw "Review output directory must be empty: $OutputDir"
}
New-Item -ItemType Directory -Path $CacheRoot, $OutputDir -Force | Out-Null

# The static SDK loader is ignored by Git; restore it from a pinned official package.
$SdkVersion = '1.0.3124.44'
$SdkHash = '31D61A59A5D5AE2EF5DCB9F175B626C1D64218217F879701EC73ED8FD74B65AE'
$SdkPackage = Join-Path $CacheRoot "Microsoft.Web.WebView2.$SdkVersion.nupkg"
if (-not (Test-Path -LiteralPath $SdkPackage -PathType Leaf)) {
    Invoke-WebRequest -Uri "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$SdkVersion/microsoft.web.webview2.$SdkVersion.nupkg" -OutFile $SdkPackage
}
if ((Get-FileHash -LiteralPath $SdkPackage -Algorithm SHA256).Hash -ne $SdkHash) {
    throw 'WebView2 SDK package hash mismatch'
}
$Loader = Join-Path $RepoRoot 'enrollment_app/webview2/lib/WebView2LoaderStatic.lib'
$Zip = [IO.Compression.ZipFile]::OpenRead($SdkPackage)
try {
    $Entry = $Zip.GetEntry('build/native/x64/WebView2LoaderStatic.lib')
    if ($null -eq $Entry) { throw 'WebView2 SDK package has no x64 static loader' }
    [IO.Compression.ZipFileExtensions]::ExtractToFile($Entry, $Loader, $true)
}
finally { $Zip.Dispose() }

$BuildName = if ($UseInstalledDependencies) { 'native-build-local' } else { 'native-build' }
$BuildDir = Join-Path $CacheRoot $BuildName
& (Join-Path $RepoRoot 'scripts/build-windows.ps1') `
    -CacheRoot $CacheRoot -BuildDir $BuildDir -InstallVcpkg:(-not $UseInstalledDependencies)

$Resources = Join-Path $RepoRoot 'installer/FaceLoginSetup/resources'
$BinaryDir = Join-Path $OutputDir 'bin'
New-Item -ItemType Directory -Path $BinaryDir -Force | Out-Null
$OwnFiles = @(
    (Join-Path $BuildDir 'face_service/Release/FaceLoginService.exe'),
    (Join-Path $BuildDir 'credential_provider/Release/FaceLoginCredentialProvider.dll'),
    (Join-Path $Resources 'FaceLoginConsole.exe'),
    (Join-Path $Resources 'FaceLoginDiag.exe'),
    (Join-Path $Resources 'FaceLoginModelProbe.exe')
)
foreach ($file in $OwnFiles) {
    Copy-Item -LiteralPath $file -Destination $BinaryDir -Force
}
$OwnNames = @($OwnFiles | ForEach-Object { Split-Path -Leaf $_ })
# vcpkg app-local deployment puts runtime dependencies beside each executable.
foreach ($directory in @($Resources, (Join-Path $BuildDir 'face_service/Release'))) {
    Get-ChildItem -LiteralPath $directory -Filter '*.dll' -File |
        Where-Object { $_.Name -notin $OwnNames } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $BinaryDir -Force }
}

$InstallerDir = Join-Path $RepoRoot 'installer/FaceLoginSetup'
Push-Location $InstallerDir
try {
    & go mod verify
    if ($LASTEXITCODE -ne 0) { throw 'Go module verification failed' }
    & go run github.com/wailsapp/wails/v2/cmd/wails@v2.13.0 build `
        -clean -platform windows/amd64 -tags uninstaller -o Uninstall.exe
    if ($LASTEXITCODE -ne 0) { throw 'Standalone uninstaller build failed' }
    Copy-Item -LiteralPath (Join-Path $InstallerDir 'build/bin/Uninstall.exe') -Destination $BinaryDir
}
finally { Pop-Location }

$LicenseDir = Join-Path $OutputDir 'licenses'
New-Item -ItemType Directory -Path $LicenseDir -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $RepoRoot 'LICENSE') -Destination (Join-Path $LicenseDir 'FaceLogin-MIT.txt')
foreach ($name in @('LICENSE.txt', 'NOTICE.txt')) {
    Copy-Item -LiteralPath (Join-Path $RepoRoot "enrollment_app/webview2/$name") `
        -Destination (Join-Path $LicenseDir "WebView2-$name")
}
$ShareDir = Join-Path $CacheRoot 'vcpkg_installed/x64-windows/share'
foreach ($package in Get-ChildItem -LiteralPath $ShareDir -Directory) {
    $notices = @(Get-ChildItem -LiteralPath $package.FullName -File |
        Where-Object { $_.Name -in @('copyright', 'vcpkg.spdx.json') })
    if ($notices.Count) {
        $destination = Join-Path $LicenseDir $package.Name
        New-Item -ItemType Directory -Path $destination -Force | Out-Null
        $notices | ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $destination }
    }
}
# Export actual module metadata and available notices rather than assuming that
# transitive Go/npm packages share the top-level project's license.
Push-Location $InstallerDir
try {
    $ModuleLines = @(& go list -m -f '{{.Path}}|{{.Version}}|{{.Dir}}' all)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inventory Go modules' }
}
finally { Pop-Location }
$GoInventory = @($ModuleLines | ForEach-Object {
    $parts = $_ -split '\|', 3
    $noticeNames = @()
    if ($parts[2] -and $parts[0] -ne 'FaceLoginSetup') {
        $destination = Join-Path $LicenseDir ('go/' + $parts[0])
        $notices = @(Get-ChildItem -LiteralPath $parts[2] -File | Where-Object {
            $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)(\..*)?$'
        })
        if ($notices.Count) {
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
            foreach ($notice in $notices) {
                Copy-Item -LiteralPath $notice.FullName -Destination $destination
                $noticeNames += $notice.Name
            }
        }
    }
    [ordered]@{ module = $parts[0]; version = $parts[1]; notices = $noticeNames }
})
$GoInventory | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $LicenseDir 'go-modules.json') -Encoding utf8

$FrontendDir = Join-Path $InstallerDir 'frontend'
$NpmLock = Get-Content (Join-Path $FrontendDir 'package-lock.json') -Raw | ConvertFrom-Json -AsHashtable
$NpmInventory = @($NpmLock.packages.Keys | Where-Object { $_ } | Sort-Object | ForEach-Object {
    $packagePath = $_
    $metadata = $NpmLock.packages[$packagePath]
    $directory = Join-Path $FrontendDir $packagePath
    $noticeNames = @()
    if (Test-Path -LiteralPath $directory -PathType Container) {
        $notices = @(Get-ChildItem -LiteralPath $directory -File | Where-Object {
            $_.Name -match '^(LICENSE|LICENCE|COPYING|NOTICE)(\..*)?$'
        })
        if ($notices.Count) {
            $destination = Join-Path $LicenseDir ('npm/' + $packagePath)
            New-Item -ItemType Directory -Path $destination -Force | Out-Null
            foreach ($notice in $notices) {
                Copy-Item -LiteralPath $notice.FullName -Destination $destination
                $noticeNames += $notice.Name
            }
        }
    }
    [ordered]@{
        packagePath = $packagePath; version = $metadata.version
        declaredLicense = $metadata.license; developmentOnly = [bool]$metadata.dev
        notices = $noticeNames
    }
})
$NpmInventory | ConvertTo-Json -Depth 5 |
    Set-Content -LiteralPath (Join-Path $LicenseDir 'npm-packages.json') -Encoding utf8
foreach ($name in @('CODE_SIGNING.md', 'PRIVACY.md', 'THIRD_PARTY.md', 'SIGNPATH_APPLICATION.md')) {
    Copy-Item -LiteralPath (Join-Path $RepoRoot $name) -Destination $OutputDir
}

$commit = (& git -C $RepoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify source commit' }
$receipt = [ordered]@{
    sourceCommit = $commit
    trackedSourceModified = [bool](& git -C $RepoRoot status --porcelain --untracked-files=no)
    reusedLocalDependencies = [bool]$UseInstalledDependencies
    runUrl = if ($env:GITHUB_RUN_ID) { "https://github.com/$env:GITHUB_REPOSITORY/actions/runs/$env:GITHUB_RUN_ID" } else { $null }
    purpose = 'Unsigned review components; not a complete installation package'
    vcpkgBaseline = (Get-Content (Join-Path $RepoRoot 'vcpkg.json') -Raw | ConvertFrom-Json).'builtin-baseline'
    webview2SdkVersion = $SdkVersion
    files = @(Get-ChildItem -LiteralPath $BinaryDir -File | Sort-Object Name | ForEach-Object {
        [ordered]@{ name = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
    })
}
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputDir 'build-receipt.json') -Encoding utf8
Write-Host "Unsigned review artifacts: $OutputDir"
