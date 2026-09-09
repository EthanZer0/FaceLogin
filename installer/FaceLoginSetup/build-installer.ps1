[CmdletBinding()]
param(
    [string]$WailsVersion = "v2.13.0"
)

$ErrorActionPreference = "Stop"

$InstallerDir = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$ResourceUninstaller = Join-Path $InstallerDir "resources\Uninstall.exe"
$WailsPackage = "github.com/wailsapp/wails/v2/cmd/wails@$WailsVersion"

Push-Location $InstallerDir
try {
    Write-Host "构建轻量独立卸载程序..."
    & go run $WailsPackage build -clean -platform windows/amd64 -tags uninstaller -o Uninstall.exe
    if ($LASTEXITCODE -ne 0) {
        throw "轻量卸载程序构建失败，退出码：$LASTEXITCODE"
    }

    $BuiltUninstaller = Join-Path $InstallerDir "build\bin\Uninstall.exe"
    if (-not (Test-Path -LiteralPath $BuiltUninstaller -PathType Leaf)) {
        throw "未找到构建出的轻量卸载程序：$BuiltUninstaller"
    }
    Copy-Item -LiteralPath $BuiltUninstaller -Destination $ResourceUninstaller -Force

    Write-Host "构建完整安装程序..."
    & go run $WailsPackage build -clean -platform windows/amd64
    if ($LASTEXITCODE -ne 0) {
        throw "完整安装程序构建失败，退出码：$LASTEXITCODE"
    }

    $BuiltSetup = Join-Path $InstallerDir "build\bin\FaceLoginSetup.exe"
    $BuiltUninstallerSize = (Get-Item -LiteralPath $ResourceUninstaller).Length
    Write-Host ("安装器构建完成：{0}" -f $BuiltSetup)
    Write-Host ("轻量卸载程序大小：{0:N2} MB" -f ($BuiltUninstallerSize / 1MB))
}
finally {
    Pop-Location
}
