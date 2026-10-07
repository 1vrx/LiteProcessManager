[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Release',
    [switch]$UserOnly,
    [string]$SdkVersion,
    [string]$MsBuildPath,
    [switch]$SmokeTest
)
$ErrorActionPreference = 'Stop'
if (-not $MsBuildPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio C++ build tools first.' }
    $MsBuildPath = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
}
if (-not $MsBuildPath -or -not (Test-Path -LiteralPath $MsBuildPath)) { throw 'MSBuild was not found.' }
$arguments = @('/nologo', '/v:minimal', '/t:Build', "/p:Configuration=$Configuration", '/p:Platform=x64')
if ($SdkVersion) { $arguments += "/p:WindowsTargetPlatformVersion=$SdkVersion" }
$projects = @('um\um.vcxproj', 'tests\tests.vcxproj')
if (-not $UserOnly) { $projects = @('vA\vA.vcxproj') + $projects }
foreach ($project in $projects) {
    & $MsBuildPath (Join-Path $PSScriptRoot $project) @arguments
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $project (exit $LASTEXITCODE)" }
}
$binaries = Join-Path $PSScriptRoot "build\x64\$Configuration"
& (Join-Path $binaries 'KpmTests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Protocol/ring tests failed.' }
& (Join-Path $binaries 'KpmTests.exe') --transport
if ($LASTEXITCODE -ne 0) { throw 'Shared-memory transport tests failed.' }
if ($SmokeTest) {
    foreach ($flags in @(@('--demo', '--smoke-test'), @('--smoke-test'))) {
        $process = Start-Process -FilePath (Join-Path $binaries 'KpmMonitor.exe') -ArgumentList $flags -WindowStyle Hidden -PassThru
        if (-not $process.WaitForExit(10000)) { $process.Kill(); throw 'GUI smoke test timed out.' }
        if ($process.ExitCode -ne 0) { throw "GUI smoke test failed: $($process.ExitCode)" }
    }
    Write-Output 'PASS: GUI startup/shutdown in demo and normal modes'
}
