# FaceLogin Model Download Script
# Downloads the 2d106det landmark model (insightface buffalo_l pack).
#
# The detection/recognition/anti-spoof models are ONNX (SCRFD det_500m,
# w600k_mbf, MiniFASNet OULU) bundled with the installer. This script only
# fetches the 106-point landmark model 2d106det.onnx, which is part of
# insightface's buffalo_l pack (also containing det_10g / w600k_r50 we do
# not need).
#
# Prerequisites: 7-Zip installed at default location for .zip extraction.

param(
    [string]$ModelsDir = "$env:ProgramData\FaceLogin\models"
)

Write-Host "============================================" -ForegroundColor Cyan
Write-Host "  FaceLogin - Model Download Script" -ForegroundColor Cyan
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "Models will be downloaded to: $ModelsDir"
Write-Host ""

# Create directory
New-Item -ItemType Directory -Force -Path $ModelsDir | Out-Null

# 2d106det.onnx is extracted from insightface's buffalo_l pack
$zipUrl   = "https://github.com/deepinsight/insightface/releases/download/v0.7/buffalo_l.zip"
$zipFile  = Join-Path $env:TEMP "buffalo_l.zip"
$extractDir = Join-Path $env:TEMP "buffalo_l_extract"
$modelName = "2d106det.onnx"
$targetFile = Join-Path $ModelsDir $modelName

if (Test-Path $targetFile) {
    Write-Host "[SKIP] $modelName already exists." -ForegroundColor Green
    $fileInfo = Get-Item $targetFile
    Write-Host "       Size: $([math]::Round($fileInfo.Length / 1MB, 1)) MB"
    exit 0
}

# Check for 7-Zip (needed to extract the .zip)
$sevenZip = "$env:ProgramFiles\7-Zip\7z.exe"
if (-not (Test-Path $sevenZip)) {
    $sevenZip = "$env:ProgramFiles(x86)\7-Zip\7z.exe"
}
if (-not (Test-Path $sevenZip)) {
    Write-Host "ERROR: 7-Zip not found at default location." -ForegroundColor Red
    Write-Host "Install 7-Zip, or manually download:"
    Write-Host "  $zipUrl"
    Write-Host "and extract $modelName into $ModelsDir"
    exit 1
}

Write-Host "[DOWNLOAD] buffalo_l.zip (contains $modelName)" -ForegroundColor Yellow
Write-Host "  URL: $zipUrl"

try {
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipFile -ErrorAction Stop
    Write-Host "  Download complete." -ForegroundColor Green

    Write-Host "  Extracting with 7-Zip..."
    if (Test-Path $extractDir) { Remove-Item $extractDir -Recurse -Force }
    & $sevenZip x "$zipFile" -o"$extractDir" -y | Out-Null

    $src = Join-Path $extractDir $modelName
    if (Test-Path $src) {
        Copy-Item $src $targetFile -Force
        Remove-Item $zipFile -Force -ErrorAction SilentlyContinue
        Remove-Item $extractDir -Recurse -Force -ErrorAction SilentlyContinue
        Write-Host "  Extraction complete. File ready." -ForegroundColor Green
        $fileInfo = Get-Item $targetFile
        Write-Host "  Size: $([math]::Round($fileInfo.Length / 1MB, 1)) MB"
    } else {
        Write-Host "  ERROR: $modelName not found in the archive." -ForegroundColor Red
        Write-Host "  The archive is at: $zipFile" -ForegroundColor Red
        Write-Host "  Extract $modelName manually into $ModelsDir" -ForegroundColor Red
    }
}
catch {
    Write-Host "  ERROR: Download failed: $_" -ForegroundColor Red
    Write-Host "  Please download manually from: $zipUrl"
    Write-Host "  and extract $modelName into $ModelsDir"
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Model download complete." -ForegroundColor Cyan
Write-Host ""
Write-Host "Models location: $ModelsDir"
Write-Host ""
Write-Host "Required files:"
Write-Host "  1. 2d106det.onnx (~5 MB)"
Write-Host "  (ONNX models det_500m.onnx / w600k_mbf.onnx / OULU_Protocol_2_model_0_0.onnx are bundled with the installer)"
Write-Host ""
Write-Host "Next step: Run the FaceLoginSetup.exe installer, or place the model in the install dir."
Write-Host "============================================" -ForegroundColor Cyan
