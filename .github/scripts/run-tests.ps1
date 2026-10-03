#
# RebootWipe CI test runner
# Run RebootWipe command-line test suite with admin privileges
#
# Design note: RebootWipe.exe outputs UTF-16 LE via _setmode(O_U16TEXT).
# PowerShell 5.1 pipelines decode child-process stdout with the system ANSI
# code page, which garbles all Unicode output. We use Start-Process with
# file-based stdout/stderr redirection to capture raw bytes that never enter
# PowerShell's encoding pipeline.
#
# Exit code mapping (RebootWipe.c wmain):
#   ParseCommand returns value X  ->  wmain returns  (X < 0 ? 1 : 0)
#   So all non-negative ParseCommand results (0, 1, ...) map to exit 0.
#   Only negative ParseCommand results (-1 specifically) map to exit 1.
#
param(
    [Parameter(Mandatory=$true)]
    [string]$ExePath,

    [Parameter(Mandatory=$false)]
    [string]$LogFile = "$env:GITHUB_WORKSPACE\test-results.log"
)

$ErrorActionPreference = "Continue"

# Skip UAC self-elevation (this script is already running elevated)
$env:CI = "true"
$env:REBOOTWIPE_SKIP_UAC = "1"

# Temp dir for per-test stdout/stderr/stdin captures
$tmpDir = Join-Path $env:TEMP "RebootWipeCI_$PID"
New-Item -ItemType Directory -Path $tmpDir -Force | Out-Null

Write-Host "=== RebootWipe Command-Line Tests ===" 2>&1 | Tee-Object $LogFile
Write-Host "Exe    : $ExePath" 2>&1 | Tee-Object -Append $LogFile
Write-Host "User   : $(whoami)" 2>&1 | Tee-Object -Append $LogFile
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "Admin  : $isAdmin" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Workdir: $($env:GITHUB_WORKSPACE)" 2>&1 | Tee-Object -Append $LogFile
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

function Invoke-ExeSafe {
    param(
        [Parameter(Mandatory=$true)]  [string]$Exe,
        [Parameter(Mandatory=$true)]  [string[]]$Args,
        [Parameter(Mandatory=$true)]  [string]$OutFile,
        [Parameter(Mandatory=$true)]  [string]$ErrFile,
        [Parameter(Mandatory=$false)] [string]$StdinInput   # text to pipe via stdin (for erase confirm)
    )

    if ($StdinInput) {
        $stdinFile = Join-Path $tmpDir "stdin_$($script:total).txt"
        Set-Content -Path $stdinFile -Value $StdinInput -NoNewline -Encoding ASCII
    } else {
        $stdinFile = $null
    }

    try {
        $spArgs = @{
            FilePath               = $Exe
            ArgumentList           = $Args
            RedirectStandardOutput = $OutFile
            RedirectStandardError  = $ErrFile
            Wait                   = $true
        }
        if ($stdinFile) { $spArgs.RedirectStandardInput = $stdinFile }

        $proc = Start-Process @spArgs -PassThru
        return $proc.ExitCode
    } catch {
        Write-Host "Invoke-ExeSafe error: $($_.Exception.Message)" 2>&1 | Tee-Object -Append $LogFile
        return 999
    }
}

function Write-CapturedOutput {
    param(
        [Parameter(Mandatory=$true)]  [string]$OutFile,
        [Parameter(Mandatory=$true)]  [string]$ErrFile,
        [Parameter(Mandatory=$false)] [string]$LogFile
    )
    if (-not $LogFile) { return }

    foreach ($f in @($OutFile, $ErrFile)) {
        if (-not (Test-Path $f)) { continue }
        try {
            [void](Add-Content -Path $LogFile -Value "--- $([System.IO.Path]::GetFileName($f)) ---" -Encoding UTF8)
            Get-Content -Path $f -Encoding Unicode -ErrorAction SilentlyContinue |
                ForEach-Object { Add-Content -Path $LogFile -Value $_ -Encoding UTF8 }
        } catch {
            Add-Content -Path $LogFile -Value "[failed to decode $f]" -Encoding UTF8
        }
    }
}

function Run-Test {
    param(
        [Parameter(Mandatory=$true)]  [string]$Name,
        [Parameter(Mandatory=$true)]  [string[]]$Args,
        [Parameter(Mandatory=$false)] [int]$ExpectedCode = 0,
        [Parameter(Mandatory=$false)] [string]$StdinInput = $null
    )

    $script:total++
    $outFile = Join-Path $tmpDir "test_${script:total}_out.txt"
    $errFile = Join-Path $tmpDir "test_${script:total}_err.txt"

    Write-Host "--- Test $script:total : $Name ---" 2>&1 | Tee-Object -Append $LogFile
    Write-Host "Command : $ExePath $($Args -join ' ')" 2>&1 | Tee-Object -Append $LogFile

    $actualCode = Invoke-ExeSafe -Exe $ExePath -Args $Args -OutFile $outFile -ErrFile $errFile -StdinInput $StdinInput

    # Dump captured output into log for artifact inspection
    Write-CapturedOutput -OutFile $outFile -ErrFile $errFile -LogFile $LogFile

    Write-Host "Exit    : $actualCode (expected: $ExpectedCode)" 2>&1 | Tee-Object -Append $LogFile

    if ($actualCode -ne $ExpectedCode) {
        Write-Host "[FAIL]  $Name" 2>&1 | Tee-Object -Append $LogFile
        $script:failures++
    } else {
        Write-Host "[PASS]  $Name" 2>&1 | Tee-Object -Append $LogFile
    }
    Write-Host "" 2>&1 | Tee-Object -Append $LogFile
}

# ======================================================================
# Exit-code cheat-sheet (see RebootWipe.c -> wmain line 72):
#   ParseCommand returns 0  -> wmain exit 0   (success)
#   ParseCommand returns 1  -> wmain exit 0   (help shown / benign)
#   ParseCommand returns -1 -> wmain exit 1   (any command failure)
#   ParseCommand never returns negative other than -1
# ======================================================================

Run-Test "help command"              @("help")                  0
Run-Test "help flag -h"              @("-h")                    0   # ParseCommand returns 1, wmain maps to 0
Run-Test "help flag /?"              @("/?")                    0   # ParseCommand returns 1, wmain maps to 0
Run-Test "read pending ops"          @("read")                  0
Run-Test "invalid command"           @("invalidcmd_xyz")        1   # ParseCommand returns -1, wmain maps to 1

# Temp file for add/skip/erase tests
$tempFile = Join-Path $env:TEMP "RebootWipeCI_$PID.tmp"
"test content for CI" | Out-File -FilePath $tempFile -Encoding ASCII
Write-Host "Created temp file: $tempFile" 2>&1 | Tee-Object -Append $LogFile

Run-Test "add temp file"             @("add", $tempFile)                     0
Run-Test "read after add"            @("read")                               0
Run-Test "skip first entry"          @("skip", "1")                          0
Run-Test "erase first entry (confirm y)" @("erase", "1") 0 -StdinInput "y`n"  # erase needs interactive y/N
Run-Test "read after erase"          @("read")                               0
Run-Test "add non-existent file"     @("add", "C:\__RW_no_such_CI__.tmp")    1   # ParseCommand returns -1, wmain maps to 1
Run-Test "skip invalid index 0"      @("skip", "0")                          1   # invalid index -> -1 -> 1
Run-Test "erase invalid 999"         @("erase", "999")                       1   # out of range -> -1 -> 1

# === Summary ===
Write-Host "=== Test Summary ===" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Total  : $total" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Passed : $($total - $failures)" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Failed : $failures" 2>&1 | Tee-Object -Append $LogFile

# Cleanup
if (Test-Path $tempFile) {
    Remove-Item $tempFile -Force -ErrorAction SilentlyContinue
}
Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue

if ($failures -gt 0) {
    Write-Error "$failures test(s) FAILED"
    exit 1
}

Write-Host "All tests PASSED!" 2>&1 | Tee-Object -Append $LogFile
exit 0
