$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $install) {
    throw 'Visual Studio C++ tools were not found.'
}

$devcmd = Join-Path $install 'Common7\Tools\VsDevCmd.bat'
$testRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceRoot = Resolve-Path (Join-Path $testRoot '..\..')
$output = Join-Path $env:TEMP 'fo4vr-workshop-draw-tests.exe'
$object = Join-Path $env:TEMP 'fo4vr-workshop-draw-tests.obj'

try {
    $command = 'call "{0}" -arch=amd64 -host_arch=amd64 >nul && cl /nologo /std:c++20 /EHsc /W4 /WX /I"{1}\src" /Fo"{2}" /Fe:"{3}" "{4}\main.cpp" && "{3}"' -f $devcmd, $sourceRoot, $object, $output, $testRoot
    & cmd.exe /d /c $command
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
finally {
    Remove-Item -LiteralPath $output, $object -Force -ErrorAction SilentlyContinue
}
