<#
.SYNOPSIS
    RebootWipe CI 命令行测试脚本
.DESCRIPTION
    以管理员权限运行 RebootWipe 的命令行测试套件
.PARAMETER ExePath
    RebootWipe.exe 的完整路径
.PARAMETER LogFile
    测试日志输出路径
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$ExePath,

    [Parameter(Mandatory=$false)]
    [string]$LogFile = "$env:GITHUB_WORKSPACE\test-results.log"
)

$ErrorActionPreference = "Stop"

# 设置 CI 环境变量跳过 UAC 提权（本脚本已经在管理员上下文中运行）
$env:CI = "true"
$env:REBOOTWIPE_SKIP_UAC = "1"

Write-Host "=== RebootWipe Command-Line Tests ===" 2>&1 | Tee-Object $LogFile
Write-Host "Exe: $ExePath" 2>&1 | Tee-Object -Append $LogFile
Write-Host "User: $(whoami)" 2>&1 | Tee-Object -Append $LogFile
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host "Is admin: $isAdmin" 2>&1 | Tee-Object -Append $LogFile
Write-Host "" 2>&1 | Tee-Object -Append $LogFile

if (-not $isAdmin) {
    Write-Error "本脚本必须以管理员权限运行！"
    exit 1
}

if (-not (Test-Path $ExePath)) {
    Write-Error "找不到可执行文件: $ExePath"
    exit 1
}

$failures = 0
$total = 0

function Run-Test {
    param(
        [Parameter(Mandatory=$true)] [string]$Name,
        [Parameter(Mandatory=$true)] [string[]]$Args,
        [Parameter(Mandatory=$false)] [int]$ExpectedCode = 0
    )

    $script:total++
    Write-Host "--- Test $script:total`: $Name ---" 2>&1 | Tee-Object -Append $LogFile
    Write-Host "Command: $ExePath $($Args -join ' ')" 2>&1 | Tee-Object -Append $LogFile

    & $ExePath @Args 2>&1 | Tee-Object -Append $LogFile
    $code = $LASTEXITCODE

    Write-Host "Exit code: $code (expected: $ExpectedCode)" 2>&1 | Tee-Object -Append $LogFile

    if ($code -ne $ExpectedCode) {
        Write-Host "[FAIL] $Name - exit code mismatch" 2>&1 | Tee-Object -Append $LogFile
        $script:failures++
    } else {
        Write-Host "[PASS] $Name" 2>&1 | Tee-Object -Append $LogFile
    }
    Write-Host "" 2>&1 | Tee-Object -Append $LogFile
}

# === 测试套件 ===

Run-Test "help command" @("help") 0
Run-Test "help flag -h" @("-h") 1
Run-Test "help flag /?" @("/?") 1
Run-Test "read pending ops (may be empty)" @("read") 0
Run-Test "invalid command" @("invalidcmd_xyz") 1

# 创建临时测试文件
$tempFile = Join-Path $env:TEMP "RebootWipeCI_$PID.tmp"
"test content for CI" | Out-File -FilePath $tempFile -Encoding ASCII
Write-Host "Created temp file: $tempFile" 2>&1 | Tee-Object -Append $LogFile

Run-Test "add temp file to reboot delete list" @("add", $tempFile) 0
Run-Test "read after add" @("read") 0
Run-Test "skip first entry" @("skip", "1") 0
Run-Test "erase first entry (physical delete)" @("erase", "1") 0
Run-Test "read after erase" @("read") 0
Run-Test "add non-existent file (should skip)" @("add", "C:\__RebootWipe_NoSuchFile_CI__.tmp") -1
Run-Test "skip invalid index 0" @("skip", "0") -1
Run-Test "erase invalid index 999" @("erase", "999") -1

# === 汇总 ===
Write-Host "=== Test Summary ===" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Total : $total" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Passed: $($total - $failures)" 2>&1 | Tee-Object -Append $LogFile
Write-Host "Failed: $failures" 2>&1 | Tee-Object -Append $LogFile

# 清理临时文件（虽然已安排重启删除，但我们手动清掉）
if (Test-Path $tempFile) {
    Remove-Item $tempFile -Force -ErrorAction SilentlyContinue
}

if ($failures -gt 0) {
    Write-Error "测试失败：$failures / $total"
    exit 1
}

Write-Host "所有测试通过！" 2>&1 | Tee-Object -Append $LogFile
exit 0
