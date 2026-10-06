# 按目标设备构建 / 烧录。
# Build or flash for a chosen board.
#
#   tools\build_board.ps1 -Board read_pico build
#   tools\build_board.ps1 -Board metalio_eink4_plus build
#   tools\build_board.ps1 -Board read_pico -Action flash -Port COM8
#
# 每块板有自己的 build 目录、sdkconfig 和芯片目标（boards\<板名>\target），三者都必须分开：
# Read Pico 是 Xtensa 的 esp32s3，Metalio 是 RISC-V 的 esp32s31，共用一个 build 目录会互相重配。
# Each board gets its own build directory, sdkconfig and chip target (boards\<name>\target). All
# three have to be separate: Read Pico is esp32s3 (Xtensa) and Metalio is esp32s31 (RISC-V), and a
# shared build directory would have them reconfigure each other.
[CmdletBinding()]
param(
    [string]$Board = 'read_pico',
    [ValidateSet('build', 'flash', 'menuconfig', 'size', 'clean', 'fullclean')]
    [string]$Action = 'build',
    [string]$Port = 'COM8',
    [switch]$NoProxy
)

# 不用 Stop：idf.py / export.ps1 会把 NOTE、进度等都写到 stderr，在 Stop 下会被提升为
# 终止错误。真正的失败用显式 throw，脚本末尾按退出码返回。
# Not Stop: idf.py and export.ps1 write NOTEs and progress to stderr, which Stop would
# promote to terminating errors. Real failures use explicit throws; the script returns the
# exit code at the end.
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$boardDir = Join-Path $repo "boards\$Board"

if (-not (Test-Path (Join-Path $boardDir 'sdkconfig.defaults'))) {
    $known = (Get-ChildItem (Join-Path $repo 'boards') -Directory | Select-Object -ExpandProperty Name) -join ', '
    throw "未知设备 '$Board'。boards\ 下有：$known"
}
$target = 'esp32s3'
$targetFile = Join-Path $boardDir 'target'
if (Test-Path $targetFile) { $target = (Get-Content $targetFile -Raw).Trim() }

# IDF_PATH 可能已被环境设成别的版本（例如残留的 v5.5.1），所以校验而不是只在未设置时赋值。
# IDF_PATH may already point at another IDF (a stale v5.5.1, say), so validate it rather than
# only assigning when it is unset.
$idfCandidate = 'D:\.espressif\v6.1\esp-idf'
if (-not $env:IDF_PATH -or -not (Test-Path (Join-Path $env:IDF_PATH 'export.ps1'))) {
    if (Test-Path (Join-Path $idfCandidate 'export.ps1')) {
        Write-Host "IDF_PATH '$env:IDF_PATH' 不可用，改用 $idfCandidate" -ForegroundColor Yellow
        $env:IDF_PATH = $idfCandidate
    } else {
        throw "找不到可用的 IDF_PATH（当前 '$env:IDF_PATH'，回退候选 '$idfCandidate' 也不存在）"
    }
}
if (-not $env:IDF_TOOLS_PATH -or -not (Test-Path $env:IDF_TOOLS_PATH)) { $env:IDF_TOOLS_PATH = 'C:\Espressif' }
$env:PYTHONUTF8 = '1'
if (-not $NoProxy) {
    # 组件管理器只在首次求解依赖时需要联网；求解结果落盘后可以关掉。
    # The component manager only needs the network for its first dependency solve.
    $env:HTTP_PROXY = 'http://127.0.0.1:7890'
    $env:HTTPS_PROXY = 'http://127.0.0.1:7890'
}

# export.ps1 会把 "Activating ESP-IDF" 写到 stderr；在 Stop 模式下那会被提升为终止错误，
# 所以这一段临时放开。 / export.ps1 writes to stderr, which ErrorActionPreference=Stop promotes
# to a terminating error, so relax it just here.
$prevEap = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
Push-Location $env:IDF_PATH
. .\export.ps1 2>&1 | Out-Null
Pop-Location
$ErrorActionPreference = $prevEap

$buildDir = Join-Path $repo "build\$Board"
$idfPy = Join-Path $env:IDF_PATH 'tools\idf.py'
# SDKCONFIG 也必须在命令行给：在 CMakeLists 里 set() 会被 IDF 自己的处理覆盖掉，结果两块板
# 共用根目录的 sdkconfig，互相把对方的 flash/PSRAM 时序写花（Read Pico 的 120MHz 就是这样丢的）。
# SDKCONFIG has to come from the command line too: setting it in CMakeLists gets overridden by
# IDF's own handling, which left both boards sharing the root sdkconfig and overwriting each
# other's timing - that is how Read Pico lost its 120 MHz flash setting.
$sdkconfig = Join-Path $repo "sdkconfig.$Board"
$args = @($idfPy, '-B', $buildDir, "-DPICO_BOARD=$Board", "-DSDKCONFIG=$sdkconfig")
if ($Action -in @('flash', 'monitor')) { $args += @('-p', $Port) }
$args += $Action

Write-Host "board=$Board target=$target build=$buildDir action=$Action" -ForegroundColor Cyan
& python @args
exit $LASTEXITCODE
