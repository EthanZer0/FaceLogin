# FaceLogin Model Download Script
# Downloads the required dlib model files for face detection and recognition.
#
# Prerequisites: 7-Zip installed at default location, or manually decompress .bz2 files.

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

# Model URLs
$models = @(
    @{
        Name = "shape_predictor_68_face_landmarks.dat"
        Url  = "http://dlib.net/files/shape_predictor_68_face_landmarks.dat.bz2"
        Size = "~61 MB compressed, ~97 MB extracted"
    },
    @{
        Name = "dlib_face_recognition_resnet_model_v1.dat"
        Url  = "http://dlib.net/files/dlib_face_recognition_resnet_model_v1.dat.bz2"
        Size = "~94 MB compressed, ~100 MB extracted"
    }
)

# Check for 7-Zip
$sevenZip = "$env:ProgramFiles\7-Zip\7z.exe"
if (-not (Test-Path $sevenZip)) {
    $sevenZip = "$env:ProgramFiles(x86)\7-Zip\7z.exe"
}
if (-not (Test-Path $sevenZip)) {
    Write-Host "WARNING: 7-Zip not found at default location." -ForegroundColor Yellow
    Write-Host "You can still download the .bz2 files manually and decompress them."
    Write-Host "Download URLs are listed below."
    $sevenZip = $null
}

foreach ($model in $models) {
    $datFile = Join-Path $ModelsDir $model.Name
    $bz2File = "$datFile.bz2"

    if (Test-Path $datFile) {
        Write-Host "[SKIP] $($model.Name) already exists." -ForegroundColor Green
        $fileInfo = Get-Item $datFile
        Write-Host "       Size: $([math]::Round($fileInfo.Length / 1MB, 1)) MB"
        continue
    }

    Write-Host "[DOWNLOAD] $($model.Name)" -ForegroundColor Yellow
    Write-Host "  URL: $($model.Url)"
    Write-Host "  Expected: $($model.Size)"

    try {
        Invoke-WebRequest -Uri $model.Url -OutFile $bz2File -ErrorAction Stop
        Write-Host "  Download complete." -ForegroundColor Green

        if ($sevenZip) {
            Write-Host "  Extracting with 7-Zip..."
            & $sevenZip e "$bz2File" -o"$ModelsDir" -y | Out-Null

            if (Test-Path $datFile) {
                Remove-Item $bz2File -Force
                Write-Host "  Extraction complete. File ready." -ForegroundColor Green
                $fileInfo = Get-Item $datFile
                Write-Host "  Size: $([math]::Round($fileInfo.Length / 1MB, 1)) MB"
            } else {
                Write-Host "  ERROR: Extraction failed. The .bz2 file is at: $bz2File" -ForegroundColor Red
                Write-Host "  Please decompress it manually." -ForegroundColor Red
            }
        } else {
            Write-Host "  Saved to: $bz2File"
            Write-Host "  Please decompress this .bz2 file manually (7-Zip or similar)."
        }
    }
    catch {
        Write-Host "  ERROR: Download failed: $_" -ForegroundColor Red
        Write-Host "  Please download manually from: $($model.Url)"
    }
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host "Model download complete." -ForegroundColor Cyan
Write-Host ""
Write-Host "Models location: $ModelsDir"
Write-Host ""
Write-Host "Required files:"
Write-Host "  1. shape_predictor_68_face_landmarks.dat (~97 MB)"
Write-Host "  2. dlib_face_recognition_resnet_model_v1.dat (~100 MB)"
Write-Host ""
Write-Host "Next step: Run install.bat as Administrator"
Write-Host "============================================" -ForegroundColor Cyan
