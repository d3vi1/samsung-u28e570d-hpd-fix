[CmdletBinding()]
param([string]$OutputJson)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$passed = New-Object 'System.Collections.Generic.List[string]'
function Check([string]$Name, [bool]$Condition) {
    if (-not $Condition) { throw "Static check failed: $Name" }
    $passed.Add($Name)
}
function Read-Xml([string]$Name) {
    $doc = New-Object System.Xml.XmlDocument
    $doc.XmlResolver = $null
    $doc.Load((Join-Path $PSScriptRoot $Name))
    return ,$doc
}
$project = Read-Xml 'MonitorFirmwareFrontend.vcxproj'
$manifest = Read-Xml 'Package.appxmanifest'
$page = Read-Xml 'MainPage.xaml'
$app = Read-Xml 'App.xaml'
Check 'four XML documents parse' ($project.DocumentElement.LocalName -eq 'Project' -and $manifest.DocumentElement.LocalName -eq 'Package' -and $page.DocumentElement.LocalName -eq 'Page' -and $app.DocumentElement.LocalName -eq 'Application')
$ns = New-Object System.Xml.XmlNamespaceManager($project.NameTable)
$ns.AddNamespace('p', 'http://schemas.microsoft.com/developer/msbuild/2003')
Check 'v110 toolset and Windows Store 8.0 target' ($project.SelectSingleNode('//p:PlatformToolset', $ns).InnerText -eq 'v110' -and $project.SelectSingleNode('//p:ApplicationTypeRevision', $ns).InnerText -eq '8.0' -and $project.SelectSingleNode('//p:AppContainerApplication', $ns).InnerText -eq 'true')
Check 'C++/CX enabled; automatic package signing disabled' ($project.SelectSingleNode('//p:CompileAsWinRT', $ns).InnerText -eq 'true' -and $project.SelectSingleNode('//p:AppxPackageSigningEnabled', $ns).InnerText -eq 'false')
$missing = @($project.SelectNodes('//p:ItemGroup/p:ClCompile | //p:ItemGroup/p:ClInclude | //p:ItemGroup/p:Page | //p:ItemGroup/p:ApplicationDefinition | //p:ItemGroup/p:AppxManifest | //p:ItemGroup/p:Image', $ns) | Where-Object { -not (Test-Path -LiteralPath (Join-Path $PSScriptRoot $_.GetAttribute('Include')) -PathType Leaf) })
Check 'all declared source and asset inputs exist' ($missing.Count -eq 0)
Check 'no engine or transport project linkage' ($project.SelectNodes('//p:ProjectReference', $ns).Count -eq 0 -and $project.OuterXml -notmatch 'monitorfw_core|nvapi|C\+\+14')
$mns = New-Object System.Xml.XmlNamespaceManager($manifest.NameTable)
$mns.AddNamespace('a', 'http://schemas.microsoft.com/appx/2010/manifest')
$identity = $manifest.SelectSingleNode('/a:Package/a:Identity', $mns)
Check 'fixed public package identity' ($identity.GetAttribute('Name') -eq 'MonitorFirmware.WinRT' -and $identity.GetAttribute('Publisher') -eq 'CN=MonitorFirmware Development')
Check 'Windows 8 manifest and no privileged extensions' ($manifest.SelectSingleNode('//a:OSMinVersion', $mns).InnerText -eq '6.2.1' -and $manifest.SelectNodes('//a:Capabilities | //a:Extensions', $mns).Count -eq 0)
$source = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'MainPage.xaml.cpp'))
$header = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'MainPage.xaml.h'))
$handlers = @($page.SelectNodes('//@Click | //@SelectionChanged') | ForEach-Object { $_.Value } | Select-Object -Unique)
$unbound = @($handlers | Where-Object { $source -notmatch ('MainPage::' + [regex]::Escape($_) + '\(') -or $header -notmatch ('void\s+' + [regex]::Escape($_) + '\(') })
Check 'all XAML event handlers declared and defined' ($unbound.Count -eq 0)
Check 'buttons use ordinary panels; C++ source is ASCII for v110' ($page.SelectNodes('//*[local-name()="WrapGrid"]').Count -eq 0 -and $source -notmatch '[^\x00-\x7F]')
$xns = New-Object System.Xml.XmlNamespaceManager($page.NameTable)
$xns.AddNamespace('x', 'http://schemas.microsoft.com/winfx/2006/xaml')
$xns.AddNamespace('ui', 'http://schemas.microsoft.com/winfx/2006/xaml/presentation')
$mode = $page.SelectSingleNode('//ui:ComboBox[@x:Name="ModeBox"]', $xns)
Check 'mock mode is the initial selection' ($mode.GetAttribute('SelectedIndex') -eq '0' -and $mode.FirstChild.GetAttribute('Tag') -eq 'mock')
$required = @('schema','request_id','created_unix','action','mode','provider_id','endpoint_locator','expected_edid_sha256','image_kind','firmware_sha256')
$absent = @($required | Where-Object { $source -notmatch ('L"' + [regex]::Escape($_) + '"') })
Check 'request contract fields and copied input names present' ($absent.Count -eq 0 -and $source.Contains('L"input.bin"') -and $source.Contains('L"backup-evidence.json"') -and $source.Contains('HashAlgorithmNames::Sha256'))
Check 'single explicit URI launch and no timers or native API access' (([regex]::Matches($source, 'Launcher::LaunchUriAsync')).Count -eq 1 -and $source -notmatch 'DispatcherTimer|ThreadPoolTimer|LoadLibrary|CreateProcess|ShellExecute|NvAPI_|QueryDisplayConfig|SetDisplayConfig')
Check 'response identity check and bounded reads present' ($source.Contains('monitorfw.response.v1') -and $source.Contains('response->GetNamedString(L"request_id"), requestId_') -and $source.Contains('unsigned maximum = 1024 * 1024') -and $source.Contains('stream->Size > maximum') -and $source.Contains('stream->Size > 0x200000'))
Check 'response mode is checked and simulation is explicit' ($source.Contains('responseMode, requestMode_') -and $source.Contains('responseProvider, requestProvider_') -and $source.Contains('MOCK / simulated'))
Check 'failed dispatch removes persistent pending identity' ($source.Contains('if (launched.get())') -and $source.Contains('values->Remove(L"request_id")') -and $source.Contains('values->Remove(L"request_action")') -and $source.Contains('values->Remove(L"request_mode")') -and $source.Contains('values->Remove(L"request_provider")'))
Check 'input publication and exports refuse overwrite' ($source.Contains('L"request.pending", CreationCollisionOption::FailIfExists') -and $source.Contains('RenameAsync(L"request.json", NameCollisionOption::FailIfExists)') -and $source.Contains('FolderPicker') -and $source.Contains('file->CopyAsync(to, file->Name, NameCollisionOption::FailIfExists)'))
$bundlePicker = [regex]::Match($source, '(?s)void MainPage::ManifestClick\(.*?(?=void MainPage::Submit)').Value
Check 'backup selection grants folder access and reads the generated manifest' ($bundlePicker.Contains('FolderPicker') -and $bundlePicker.Contains('PickSingleFolderAsync') -and $bundlePicker.Contains('ReadTextBounded(folder, L"backup.json", 65536)') -and $bundlePicker -notmatch 'FileOpenPicker|PickSingleFileAsync' -and $header.Contains('StorageFolder^ evidenceFolder_'))
Check 'bundle references are bounded safe leaves and case-insensitively distinct' ($source.Contains('value->Length() > 255') -and $source.Contains('!IsAsciiAlphanumeric(leaf[0])') -and $source.Contains('leaf[leaf.size() - 1] == L''.''') -and $source.Contains('stem == L"con"') -and $source.Contains('stem.substr(0, 3) == L"com"') -and $source.Contains('LowerAscii(firstName) == LowerAscii(secondName)') -and $source.Contains('LowerAscii(firstName) == LowerAscii(sfdpName)') -and $source.Contains('LowerAscii(secondName) == LowerAscii(sfdpName)'))
Check 'both reads and SFDP are copied and hashed with exact lengths' ($source.Contains('CreateFolderAsync(L"evidence", CreationCollisionOption::FailIfExists)') -and $source.Contains('CopyEvidenceFile(source, evidence, first->GetNamedString(L"file"), 0x200000') -and $source.Contains('CopyEvidenceFile(source, evidence, second->GetNamedString(L"file"), 0x200000') -and $source.Contains('CopyEvidenceFile(source, evidence, flash->GetNamedString(L"sfdp_file"), 64') -and $source.Contains('stream->Size != exactSize') -and $source.Contains('!Same(hash, expectedHash)'))
$submit = [regex]::Match($source, '(?s)void MainPage::Submit\(.*?(?=void MainPage::ApplyResponse)').Value
Check 'bundle and selected image verification complete before request publication' ($submit.IndexOf('CopyEvidenceBundle(evidence, folder)') -ge 0 -and $submit.IndexOf('CopyEvidenceBundle(evidence, folder)') -lt $submit.IndexOf('HashCopiedFile(folder, L"input.bin"') -and $submit.IndexOf('HashCopiedFile(folder, L"input.bin"') -lt $submit.IndexOf('PublishRequest(folder, request)') -and $submit.Contains('!Same(hash, evidenceHash)') -and $submit.IndexOf('PublishRequest(folder, request)') -lt $submit.IndexOf('Launcher::LaunchUriAsync'))
Check 'C++/CX IsEnabled setters are assigned separately' ($source -notmatch 'IsEnabled\s*=\s*\w+->IsEnabled')
$dimensions = @{ 'Logo.png'=@(150,150); 'SmallLogo.png'=@(30,30); 'StoreLogo.png'=@(50,50); 'SplashScreen.png'=@(620,300) }
foreach ($name in $dimensions.Keys) {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot ('Assets/' + $name)))
    $width = ([int]$bytes[16] -shl 24) -bor ([int]$bytes[17] -shl 16) -bor ([int]$bytes[18] -shl 8) -bor [int]$bytes[19]
    $height = ([int]$bytes[20] -shl 24) -bor ([int]$bytes[21] -shl 16) -bor ([int]$bytes[22] -shl 8) -bor [int]$bytes[23]
    Check ('PNG dimensions ' + $name) ($bytes[0] -eq 137 -and $bytes[1] -eq 80 -and $width -eq $dimensions[$name][0] -and $height -eq $dimensions[$name][1])
}
$hashes = [ordered]@{}
foreach ($name in @('MonitorFirmwareFrontend.vcxproj','Package.appxmanifest','App.xaml','App.xaml.cpp','App.xaml.h','MainPage.xaml','MainPage.xaml.cpp','MainPage.xaml.h','pch.h','pch.cpp','Test-WinRtStatic.ps1')) {
    $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$result = [ordered]@{ schema='monitorfw.winrt-static-tests.v1'; passed=$passed.Count; checks=@($passed); source_hashes=$hashes; compile_performed=$false; xaml_compiler_performed=$false; sdk_schema_validation_performed=$false; runtime_test_performed=$false; deployment_performed=$false; broker_launched=$false; hardware_accessed=$false }
$json = $result | ConvertTo-Json -Depth 5
if ($OutputJson) { [IO.File]::WriteAllText([IO.Path]::GetFullPath($OutputJson), $json + [Environment]::NewLine, (New-Object System.Text.UTF8Encoding($false))) }
$json
