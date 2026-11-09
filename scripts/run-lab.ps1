param(
    [ValidateSet('simulate','benchmark','scale','experiment','test','all')]
    [string]$Mode = 'simulate',
    [ValidateSet('calm','rally','shock')]
    [string]$Scenario = 'shock',
    [switch]$Portable
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
Push-Location -LiteralPath $projectRoot
try {
    $buildName = if ($Portable) { 'portable' } else { 'release' }
    $buildPath = Join-Path $projectRoot "build\$buildName"
    $nativeOption = if ($Portable) { 'OFF' } else { 'ON' }
    $cmakeArgs = @('-S', $projectRoot, '-B', $buildPath, '-DCMAKE_BUILD_TYPE=Release', "-DLOB_NATIVE=$nativeOption")
    if (-not (Test-Path -LiteralPath (Join-Path $buildPath 'CMakeCache.txt'))) {
        if (Get-Command ninja -ErrorAction SilentlyContinue) { $cmakeArgs += @('-G', 'Ninja') }
        $compiler = Get-Command g++ -ErrorAction SilentlyContinue
        if ($compiler) { $cmakeArgs += "-DCMAKE_CXX_COMPILER=$($compiler.Source.Replace('\','/'))" }
    }
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
    & cmake --build $buildPath --config Release --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Compilation failed.' }
    $executable = Join-Path $buildPath 'limit_order_book.exe'
    if (-not (Test-Path -LiteralPath $executable)) { $executable = Join-Path $buildPath 'Release\limit_order_book.exe' }
    function Invoke-LabCommand([string[]]$CommandArguments) {
        & $executable @CommandArguments
        if ($LASTEXITCODE -ne 0) { throw "Command failed: $CommandArguments" }
    }
    if ($Mode -eq 'test' -or $Mode -eq 'all') {
        & ctest --test-dir $buildPath -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
    if ($Mode -eq 'benchmark' -or $Mode -eq 'all') {
        Invoke-LabCommand -CommandArguments @('benchmark', '--output', 'reports/benchmark.csv')
    }
    if ($Mode -eq 'scale' -or $Mode -eq 'all') {
        Invoke-LabCommand -CommandArguments @('scale', '--orders', '500000', '--workers', '8', '--output', 'reports/scaling.csv')
    }
    if ($Mode -eq 'experiment' -or $Mode -eq 'all') {
        Invoke-LabCommand -CommandArguments @('experiment', '--scenario', $Scenario, '--seeds', '30', '--output', "reports/experiment-$Scenario.csv")
    }
    if ($Mode -eq 'simulate' -or $Mode -eq 'all') {
        Invoke-LabCommand -CommandArguments @('simulate', '--scenario', $Scenario, '--output', 'reports/replay.html')
        Start-Process -FilePath (Join-Path $projectRoot 'reports\replay.html')
    }
}
finally { Pop-Location }
