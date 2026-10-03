#
# RebootWipe CI test runner
# Run RebootWipe command-line test suite with admin privileges
#
# Design note: RebootWipe.exe outputs UTF-16 LE via _setmode(O_U16TEXT).
# PowerShell 5.1 pipelines decode child-process stdout with the system ANSI
# code page, which garbles all Unicode output (Chinese, box-drawing chars).
# We use Start-Process with file-based stdout/stderr redirection to capture
# raw bytes that never enter PowerShell's encoding pipeline. Exit code is
# what matters for CI; raw output can be inspected from the artifact log.
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

# Temp dir for per-test stdout/stderr captures
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
        [Parameter(Mandatory=$true)] [string]$Exe,
        [Parameter(Mandatory=$true)] [string[]]$Args,
        [Parameter(Mandatory=$true)] [string]$OutFile,
        [Parameter(Mandatory=$true)] [string]$ErrFile
    )
    # Start-Process with RedirectStandardOutput/Error writes raw bytes to
    # files without PowerShell encoding intervention. This preserves UTF-16 LE
    # that RebootWipe.exe emits (Chinese + box-drawing Unicode characters).
    $argStr = $Args -join ' '
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $argStr
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $proc.WaitForExit()
    return $proc.ExitCode
}

function Write-CapturedOutput {
    param(
        [Parameter(Mandatory=$true)] [string]$OutFile,
        [Parameter(Mandatory=$true)] [string]$ErrFile,
        [Parameter(Mandatory=$false)] [string]$LogFile
    )
    if (-not $LogFile) { return }
    # Try UTF-16 LE first (RebootWipe.exe output mode), then UTF-8 fallback
    foreach ($f in @($OutFile, $ErrFile)) {
        if (Test-Path $f) {
            try {
                $bytes = [System.IO.File]::ReadAllBytes($f)
                if ($bytes.Length -ge 2) {
                    # Check UTF-16 LE BOM or plausible pattern
                    $isUtf16 = ($bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) -or
                               (($bytes.Length % 2 -eq 0) -and ($bytes | Select-Object -Index 1 -First 1) -eq 0)
                    if ($isUtf16) {
                        $text = [System.Text.Encoding]::Unicode.GetString($bytes)
                    } else {
                        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
                    }
                    Add-Content -Path $LogFile -Value $text -Encoding UTF8
                }
            } catch {
                # If anything fails, just dump raw line count
                $lines = (Get-Content $f -ErrorAction SilentlyContinue | Measure-Object -Line).Lines
                Add-Content -Path $LogFile -Value "[raw output: $lines lines, skipped decode]"
            }
        }
    }
}

function Run-Test {
    param(
        [Parameter(Mandatory=$true)]  [string]$Name,
        [Parameter(Mandatory=$true)]  [string[]]$Args,
        [Parameter(Mandatory=$false)] [int]$ExpectedCode = 0
    )

    $script:total++
    $outFile = Join-Path $tmpDir "test_${script:total}_out.bin"
    $errFile = Join-Path $tmpDir "test_${script:total}_err.bin"

    Write-Host "--- Test $script:total : $Name ---" 2>&1 | Tee-Object -Append $LogFile
    Write-Host "Command : $ExePath $($Args -join ' ')" 2>&1 | Tee-Object -Append $LogFile

    $actualCode = Invoke-ExeSafe -Exe $ExePath -Args $Args -OutFile $outFile -ErrFile $errFile

    # Dump captured output into log for artifact inspection (best-effort decode)
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

# === Test Suite ===

Run-Test "help command"              @("help")                    0
Run-Test "help flag -h"              @("-h")                      1
Run-Test "help flag /?"              @("/?")                      1
Run-Test "read pending ops"          @("read")                    0
Run-Test "invalid command"           @("invalidcmd_xyz")          1

# Temp file for add/skip/erase tests
$tempFile = Join-Path $env:TEMP "RebootWipeCI_$PID.tmp"
"test content for CI" | Out-File -FilePath $tempFile -Encoding ASCII
Write-Host "Created temp file: $tempFile" 2>&1 | Tee-Object -Append $LogFile

Run-Test "add temp file"             @("add", $tempFile)                    0
Run-Test "read after add"            @("read")                              0
Run-Test "skip first entry"          @("skip", "1")                         0
Run-Test "erase first entry"         @("erase", "1")                        0
Run-Test "read after erase"          @("read")                              0
Run-Test "add non-existent file"     @("add", "C:\__RW_no_such_CI__.tmp")  -1
Run-Test "skip invalid index 0"      @("skip", "0")                         -1
Run-Test "erase invalid 999"         @("erase", "999")                      -1

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
