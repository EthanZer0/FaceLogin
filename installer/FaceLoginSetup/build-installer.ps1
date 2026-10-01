[CmdletBinding()]
param(
    [string]$WailsVersion = "v2.13.0",
    [switch]$ReleaseSigning,
    [string]$CertificateThumbprint = "",
    [ValidateSet("CurrentUser", "LocalMachine")]
    [string]$CertificateStore = "CurrentUser",
    [string]$SignToolPath = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)

$ErrorActionPreference = "Stop"

$InstallerDir = (Resolve-Path -LiteralPath $PSScriptRoot).Path
$ResourceUninstaller = Join-Path $InstallerDir "resources\Uninstall.exe"
$WailsPackage = "github.com/wailsapp/wails/v2/cmd/wails@$WailsVersion"
$SigningContext = $null
if ($ReleaseSigning) {
    . (Join-Path $InstallerDir "sign-windows.ps1")
    $SigningContext = New-WindowsSigningContext -CertificateThumbprint $CertificateThumbprint `
        -CertificateStore $CertificateStore -SignToolPath $SignToolPath -TimestampUrl $TimestampUrl
    # 组件编译及同步必须在调用本脚本前完成；卸载器稍后重新构建、签名。
    $PayloadFiles = @(Get-ChildItem -LiteralPath (Join-Path $InstallerDir "resources") -Recurse -File |
        Where-Object { $_.Extension -in @('.exe', '.dll') -and $_.FullName -ne $ResourceUninstaller } |
        Sort-Object FullName | Select-Object -ExpandProperty FullName)
    if ($PayloadFiles.Count -eq 0) { throw "安装资源中未找到待签名的组件。" }
    Invoke-WindowsSigning -Context $SigningContext -Paths $PayloadFiles
}
else {
    if ($CertificateThumbprint -or $SignToolPath) {
        throw "已提供签名参数，请同时指定 -ReleaseSigning，避免意外生成未签名安装包。"
    }
    Write-Warning "当前为开发构建，不执行发布签名。正式发布请指定 -ReleaseSigning。"
}

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
    if ($ReleaseSigning) {
        Invoke-WindowsSigning -Context $SigningContext -Paths @($BuiltUninstaller)
    }
    Copy-Item -LiteralPath $BuiltUninstaller -Destination $ResourceUninstaller -Force

    Write-Host "构建完整安装程序..."
    & go run $WailsPackage build -clean -platform windows/amd64
    if ($LASTEXITCODE -ne 0) {
        throw "完整安装程序构建失败，退出码：$LASTEXITCODE"
    }

    $BuiltSetup = Join-Path $InstallerDir "build\bin\FaceLoginSetup.exe"
    if (-not (Test-Path -LiteralPath $BuiltSetup -PathType Leaf)) {
        throw "未找到构建出的安装程序：$BuiltSetup"
    }
    if ($ReleaseSigning) {
        Invoke-WindowsSigning -Context $SigningContext -Paths @($BuiltSetup)
        # 打包结束后再次验证全部负载，确保签名未在构建过程中被覆盖。
        foreach ($file in @($PayloadFiles) + @($ResourceUninstaller, $BuiltSetup)) {
            Assert-WindowsSignature -Context $SigningContext -Path $file
        }
    }
    $SetupHash = (Get-FileHash -LiteralPath $BuiltSetup -Algorithm SHA256).Hash
    $BuiltUninstallerSize = (Get-Item -LiteralPath $ResourceUninstaller).Length
    Write-Host ("安装器构建完成：{0}" -f $BuiltSetup)
    Write-Host ("轻量卸载程序大小：{0:N2} MB" -f ($BuiltUninstallerSize / 1MB))
    Write-Host ("安装器 SHA256：{0}" -f $SetupHash)
}
finally {
    Pop-Location
}
