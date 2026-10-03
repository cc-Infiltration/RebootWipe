#
# RebootWipe CI test runner
# Run RebootWipe command-line test suite with admin privileges
#
param(
    [Parameter(Mandatory=$true)]
    [string]$ExePath,

    [Parameter(Mandatory=$false)]
    [string]$LogFile = "$env:GITHUB_WORKSPACE\test-results.log"
)

$ErrorActionPreference = "Stop"

# Skip UAC self-elevation (this script is already running elevated)
$env:CI = "true"
$env:REBOOTWIPE_SKIP_UAC = "1"

Write-Host "=== RebootWipe Command-Line Tests ===" 2>&1 | Tee-Object $LogFile
Write-Host "Exe : $ExePath" 2>&1 | Tee-Object -Append $LogFile
Write-Host "User: $(whoami)" 2>&1 | Tee-Object -Append $LogFile
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "Admin: $isAdmin" 2>&1 | Tee-Object -Append $LogFile
Write-Host "" 2>&1 | Tee-Object -Append $LogFile

if (-not $isAdmin) {
    Write-Error "This script must run as Administrator!"
    exit 1
}

if (-not (Test-Path $ExePath)) {
    Write-Error "Executable not found: $ExePath"
    exit 1
}

$failures = 0
$total = 0

function Run-Test {
    param(
        [Parameter(Mandatory=$true)]  [string]$Name,
        [Parameter(Mandatory=$true)]  [string[]]$Args,
        [Parameter(Mandatory=$false)] [int]$ExpectedCode = 0
    )

    $script:total++
    Write-Host "--- Test $script:total : $Name ---" 2>&1 | Tee-Object -Append $LogFile
    Write-Host "Command: $ExePath $($Args -join ' ')" 2>&1 | Tee-Object -Append $LogFile

    & $ExePath @Args 2>&1 | Tee-Object -Append $LogFile
    $code = $LASTEXITCODE

    Write-Host "Exit code: $code (expected: $ExpectedCode)" 2>&1 | Tee-Object -Append $LogFile

    if ($code -ne $ExpectedCode) {
        Write-Host "[FAIL] $Name" 2>&1 | Tee-Object -Append $LogFile
        $script:failures++
    } else {
        Write-Host "[PASS] $Name" 2>&1 | Tee-Object -Append $LogFile
    }
    Write-Host "" 2>&1 | Tee-Object -Append $LogFile
}

# === Test Suite ===

Run-Test "help command"            @("help")         0
Run-Test "help flag -h"            @("-h")           1
Run-Test "help flag /?"            @("/?")           1
Run-Test "read pending ops"        @("read")         0
Run-Test "invalid command"         @("invalidcmd_xyz") 1

# Create temp file for add/skip/erase tests
$tempFile = Join-Path $env:TEMP "RebootWipeCI_$PID.tmp"
"test content for CI" | Out-File -FilePath $tempFile -Encoding ASCII
Write-Host "Created temp file: $tempFile" 2>&1 | Tee-Object -Append $LogFile

Run-Test "add temp file"           @("add", $tempFile)           0
Run-Test "read after add"          @("read")                     0
Run-Test "skip first entry"        @("skip", "1")                0
Run-Test "erase first entry"       @("erase", "1")               0
Run-Test "read after erase"        @("read")                     0
Run-Test "add non-existent file"   @("add", "C:\__RW_no_such_file_CI__.tmp") -1
Run-Test "skip invalid index 0"    @("skip", "0")                -1
Run-Test "erase invalid 999"       @("erase", "999")             -1

# === Summary ===
Write-Host "=== Test Summary ===" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Total  : $total" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Passed : $($total - $failures)" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Failed : $failures" 2>&1 | Tee-Object -Append $LogFile

# Cleanup
if (Test-Path $tempFile) {
    Remove-Item $tempFile -Force -ErrorAction SilentlyContinue
}

if ($failures -gt 0) {
    Write-Error "$failures test(s) FAILED"
    exit 1
}

Write-Host "All tests PASSED!" 2>&1 | Tee-Object -Append $LogFile
exit 0
