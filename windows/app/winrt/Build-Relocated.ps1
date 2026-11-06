#requires -Version 3.0
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$ToolchainRoot,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build\relocated-x86'),
    [switch]$CreateUnsignedPackage
)
$ErrorActionPreference = 'Stop'
$relocated = (Resolve-Path -LiteralPath $ToolchainRoot).Path
$buildRoot = [IO.Path]::GetFullPath($OutputDirectory)
$vsRoot = Join-Path $relocated 'Program Files\Microsoft Visual Studio 11.0'
$vcRoot = Join-Path $vsRoot 'VC'
$sdkRoot = Join-Path $relocated 'Windows Kits\8.0'
$msbuildRoot = Join-Path $relocated 'Program Files\MSBuild'
$cppRoot = Join-Path $msbuildRoot 'Microsoft.Cpp\v4.0\V110'
$taskDll = Join-Path $cppRoot 'Microsoft.Build.CPPTasks.Common.v110.dll'
if (-not (Test-Path -LiteralPath $taskDll)) {
    $taskDll = Join-Path $vsRoot 'Common7\IDE\PublicAssemblies\Microsoft.Build.CPPTasks.Common.v110.dll'
}
$msbuildExe = Join-Path $env:SystemRoot 'Microsoft.NET\Framework\v4.0.30319\MSBuild.exe'
foreach ($required in @($taskDll,$msbuildExe,(Join-Path $vcRoot 'bin\cl.exe'),(Join-Path $sdkRoot 'References\CommonConfiguration\Neutral\Windows.winmd'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw ('Missing build component: ' + $required) }
}
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$taskNames = @('SetEnv','VCMessage','LIB','MIDL','RC','Mt','XSD','XDCMake','BscMake','CustomBuild','CL','Link','FXC','CppClean','METAGEN','MIBPreProcessDependencyGraph','MIBPostProcessDependencyGraph')
$taskElements = foreach ($name in $taskNames) { '<UsingTask TaskName="' + $name + '" AssemblyFile="' + [Security.SecurityElement]::Escape($taskDll) + '" />' }
$overridePath = Join-Path $buildRoot 'LocalCppTasks.targets'
$xml = '<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">' + [Environment]::NewLine + ($taskElements -join [Environment]::NewLine) + [Environment]::NewLine + '</Project>'
[IO.File]::WriteAllText($overridePath,$xml,(New-Object Text.UTF8Encoding($false)))
$savedPath = $env:PATH
$savedExtensions = $env:MSBuildExtensionsPath
$savedExtensions32 = $env:MSBuildExtensionsPath32
$buildExitCode = 1
try {
    $env:PATH = (Join-Path $vcRoot 'bin') + ';' + (Join-Path $vcRoot 'vcpackages') + ';' + (Join-Path $vsRoot 'Common7\IDE') + ';' + (Join-Path $vcRoot 'redist\x86\Microsoft.VC110.CRT') + ';' + (Join-Path $sdkRoot 'bin\x86') + ';' + $savedPath
    $env:MSBuildExtensionsPath = $msbuildRoot
    $env:MSBuildExtensionsPath32 = $msbuildRoot
    $properties = [ordered]@{
        Configuration='Release'; Platform='Win32'; VisualStudioVersion='11.0'
        VCTargetsPath=($cppRoot+'\'); VCTargetsPath11=($cppRoot+'\')
        VSInstallDir=($vsRoot+'\'); VCInstallDir=($vcRoot+'\')
        VSInstallDir_110=($vsRoot+'\'); VCInstallDir_110=($vcRoot+'\')
        WindowsSdkDir=($sdkRoot+'\'); WindowsSdkDir_80=($sdkRoot+'\')
        WindowsSDK80Path=($sdkRoot+'\'); TargetPlatformSdkPath=($sdkRoot+'\')
        SDKReferenceDirectoryRoot=(Join-Path $relocated 'Program Files\Microsoft SDKs')
        TargetPlatformWinMDLocation=(Join-Path $sdkRoot 'References\CommonConfiguration\Neutral')
        MakePriExeFullPath=(Join-Path $sdkRoot 'bin\x86\makepri.exe')
        ForceImportBeforeCppTargets=$overridePath; TrackFileAccess='false'
        OutDir=(Join-Path $buildRoot 'bin\'); IntDir=(Join-Path $buildRoot 'obj\')
        AppxPackageSigningEnabled='false'
        GenerateAppxPackageOnBuild=([string][bool]$CreateUnsignedPackage).ToLowerInvariant()
        XamlTargetsPath=(Join-Path $msbuildRoot 'Microsoft\WindowsXaml\v11.0\')
        AppxPackageTargets=(Join-Path $msbuildRoot 'Microsoft\VisualStudio\v11.0\AppxPackage\Microsoft.AppxPackage.Targets')
    }
    $arguments = @((Join-Path $PSScriptRoot 'MonitorFirmwareFrontend.vcxproj'), '/nologo', '/m:1', '/v:minimal', '/t:Build')
    foreach ($key in $properties.Keys) { $arguments += ('/p:' + $key + '=' + $properties[$key]) }
    & $msbuildExe @arguments 2>&1 | Tee-Object -FilePath (Join-Path $buildRoot 'build.log')
    $buildExitCode = $LASTEXITCODE
} finally {
    $env:PATH = $savedPath
    $env:MSBuildExtensionsPath = $savedExtensions
    $env:MSBuildExtensionsPath32 = $savedExtensions32
}
exit $buildExitCode
