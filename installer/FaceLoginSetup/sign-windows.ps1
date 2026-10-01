$ErrorActionPreference = "Stop"

function New-WindowsSigningContext {
    param(
        [string]$CertificateThumbprint,
        [ValidateSet("CurrentUser", "LocalMachine")][string]$CertificateStore,
        [string]$SignToolPath,
        [string]$TimestampUrl
    )

    $thumbprint = ($CertificateThumbprint -replace '\s', '').ToUpperInvariant()
    if ($thumbprint -notmatch '^[0-9A-F]{40}$') {
        throw "发布签名需要有效的代码签名证书指纹（40 位十六进制）。"
    }
    $certificatePath = "Cert:\$CertificateStore\My\$thumbprint"
    if (-not (Test-Path -LiteralPath $certificatePath)) {
        throw "未找到代码签名证书：$certificatePath"
    }
    $certificate = Get-Item -LiteralPath $certificatePath
    if (-not $certificate.HasPrivateKey -or
        $certificate.NotBefore -gt (Get-Date) -or $certificate.NotAfter -le (Get-Date) -or
        $certificate.EnhancedKeyUsageList.ObjectId -notcontains '1.3.6.1.5.5.7.3.3') {
        throw "证书必须在有效期内、具有私钥，并允许代码签名。"
    }
    if ($certificate.Subject -eq $certificate.Issuer) {
        throw "正式发布不能使用自签名证书，请配置可信代码签名证书。"
    }
    $chain = New-Object System.Security.Cryptography.X509Certificates.X509Chain
    try {
        $chain.ChainPolicy.ApplicationPolicy.Add(
            (New-Object System.Security.Cryptography.Oid '1.3.6.1.5.5.7.3.3'))
        if (-not $chain.Build($certificate)) {
            throw "代码签名证书信任链验证失败：$($chain.ChainStatus.Status -join ', ')"
        }
    }
    finally { $chain.Dispose() }

    if ([string]::IsNullOrWhiteSpace($SignToolPath)) {
        $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
        if ($null -ne $command) {
            $SignToolPath = $command.Source
        }
        else {
            $sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
            $SignToolPath = Get-ChildItem -LiteralPath $sdkBin -Directory |
                Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
                Sort-Object { [version]$_.Name } -Descending |
                ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
                Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
                Select-Object -First 1
        }
    }
    if ([string]::IsNullOrWhiteSpace($SignToolPath) -or
        -not (Test-Path -LiteralPath $SignToolPath -PathType Leaf)) {
        throw "未找到 SignTool，请安装 Windows SDK 或指定 -SignToolPath。"
    }
    $timestampUri = $null
    if (-not [Uri]::TryCreate($TimestampUrl, [UriKind]::Absolute, [ref]$timestampUri) -or
        $timestampUri.Scheme -notin @('http', 'https')) {
        throw "时间戳地址必须是有效的 HTTP/HTTPS RFC 3161 服务地址。"
    }
    return @{
        Tool = (Resolve-Path -LiteralPath $SignToolPath).Path
        Thumbprint = $thumbprint
        MachineStore = ($CertificateStore -eq 'LocalMachine')
        TimestampUrl = $TimestampUrl
    }
}

function Assert-WindowsSignature {
    param([hashtable]$Context, [string]$Path)

    & $Context.Tool verify /pa /all /q $Path
    if ($LASTEXITCODE -ne 0) {
        throw "签名验证失败：$Path（退出码：$LASTEXITCODE）"
    }
}

function Invoke-WindowsSigning {
    param([hashtable]$Context, [string[]]$Paths)

    foreach ($path in $Paths) {
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        if ($signature.Status -eq 'Valid') {
            Assert-WindowsSignature -Context $Context -Path $path
            Write-Host "保留有效签名：$path"
            continue
        }
        if ($signature.Status -ne 'NotSigned') {
            throw "文件存在无效签名，停止发布而不覆盖：$path（$($signature.Status)）"
        }
        $arguments = @('sign', '/sha1', $Context.Thumbprint, '/s', 'My',
            '/fd', 'SHA256', '/tr', $Context.TimestampUrl, '/td', 'SHA256')
        if ($Context.MachineStore) { $arguments += '/sm' }
        Write-Host "签名：$path"
        & $Context.Tool @arguments $path
        if ($LASTEXITCODE -ne 0) {
            throw "签名或时间戳失败：$path（退出码：$LASTEXITCODE）"
        }
        $signed = Get-AuthenticodeSignature -LiteralPath $path
        if ($null -eq $signed.TimeStamperCertificate) {
            throw "签名缺少时间戳，停止发布：$path"
        }
        Assert-WindowsSignature -Context $Context -Path $path
    }
}
