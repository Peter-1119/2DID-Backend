<#
.SYNOPSIS
    Automatically bundles MSYS2 DLL dependencies for a C++ executable.
.DESCRIPTION
    1. Scans the executable using `ldd`.
    2. Filters out Windows system DLLs.
    3. Forces copying DLLs from the MSYS2 UCRT64 bin folder to ensure
       no Anaconda/System pollution occurs.
.EXAMPLE
    .\Bundle-MsysApp.ps1 -Target ".\lpsm_app.exe"
#>

param (
    [Parameter(Mandatory=$true)]
    [string]$Target,

    [string]$MsysBin = "C:\msys64\ucrt64\bin",
    [string]$OutputDir = "dist"
)

# 1. Check Requirements
if (-not (Test-Path $Target)) {
    Write-Host "[ERROR] Target file '$Target' not found!" -ForegroundColor Red
    exit 1
}

# 2. Prepare Output Directory
$ExeName = [System.IO.Path]::GetFileName($Target)
$FinalOutDir = Join-Path $OutputDir ($ExeName -replace '\.exe','')

if (-not (Test-Path $FinalOutDir)) {
    New-Item -ItemType Directory -Path $FinalOutDir | Out-Null
}

Write-Host "==========================================" -ForegroundColor Cyan
Write-Host "  Bundling Dependencies for: $ExeName" -ForegroundColor Cyan
Write-Host "  Source (MSYS2): $MsysBin" -ForegroundColor Gray
Write-Host "  Output: $FinalOutDir" -ForegroundColor Gray
Write-Host "==========================================" -ForegroundColor Cyan

# 3. Copy the Main Executable
Copy-Item -Path $Target -Destination $FinalOutDir -Force
Write-Host "Copied Main Executable: $ExeName" -ForegroundColor Green

# 4. Run ldd and Parse
Write-Host "Scanning dependencies..." -ForegroundColor Yellow
$LddOutput = ldd $Target

# Use a HashSet to avoid duplicate copies
$ProcessedDlls = New-Object System.Collections.Generic.HashSet[string]

foreach ($line in $LddOutput) {
    # Regex to capture "  libname.dll => "
    if ($line -match '^\s+([^\s]+\.dll)\s=>') {
        $DllName = $matches[1]

        # Skip if already processed
        if ($ProcessedDlls.Contains($DllName)) { continue }
        $ProcessedDlls.Add($DllName) | Out-Null

        # 5. The "Sanitization" Logic
        # We DO NOT trust the path ldd gives us (it might be Anaconda).
        # We ONLY check if this DLL exists in MSYS2/bin.
        
        $MsysPath = Join-Path $MsysBin $DllName

        if (Test-Path $MsysPath) {
            # It exists in MSYS2 -> It is a dependency we need to bundle
            Copy-Item -Path $MsysPath -Destination $FinalOutDir -Force
            Write-Host "  [+] Bundled: $DllName" -ForegroundColor Green
        }
        else {
            # It does not exist in MSYS2 -> Likely a Windows System DLL (kernel32, etc.)
            # Write-Host "  [-] Skipped (System): $DllName" -ForegroundColor DarkGray
        }
    }
}

# 6. Copy Config files (Optional but usually needed)
$ConfigJson = "config.json"
if (Test-Path $ConfigJson) {
    Copy-Item -Path $ConfigJson -Destination $FinalOutDir -Force
    Write-Host "  [+] Config Copied: $ConfigJson" -ForegroundColor Magenta
}

Write-Host "`nDone! Your portable app is in: $FinalOutDir" -ForegroundColor Cyan