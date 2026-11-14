param(
    [string]$BuildPath = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-mingw-x64'),
    [string]$AimpPath,
    [string]$ReceiverIP,
    [string]$ProfilePath = (Join-Path $env:APPDATA 'AIMP\AirPlay.ini'),
    [switch]$CheckFirewallOnly,
    [switch]$AllowFirewall
)
$ErrorActionPreference = 'Stop'
function Get-AimpInstallations {
    $candidates = @(
        foreach ($key in @(
            'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\AIMP.exe',
            'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\AIMP.exe',
            'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\AIMP.exe'
        )) {
            $entry = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
            if ($entry -and $entry.'(default)') { Split-Path -Parent ($entry.'(default)'.Trim('"')) }
        }
        foreach ($key in @(
            'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
            'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
            'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*'
        )) {
            foreach ($entry in Get-ItemProperty -Path $key -ErrorAction SilentlyContinue | Where-Object { $_.DisplayName -match '^AIMP(?:\s|$)' }) {
                if ($entry.InstallLocation) { $entry.InstallLocation.Trim('"') }
                if ($entry.DisplayIcon) {
                    $icon = ($entry.DisplayIcon -replace ',\s*-?\d+\s*$', '').Trim('"')
                    Split-Path -Parent $icon
                }
            }
        }
        foreach ($root in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432)) {
            if ($root) { Join-Path $root 'AIMP' }
        }
    )
    $candidates | Where-Object { $_ -and (Test-Path -LiteralPath ([IO.Path]::Combine($_, 'AIMP.exe')) -PathType Leaf) } |
        ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\') } | Sort-Object -Unique
}
function Select-AimpFolder {
    param([string[]]$Candidates)
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    try {
        $dialog.Title = 'Select AIMP.exe'
        $dialog.Filter = 'AIMP.exe|AIMP.exe'
        $dialog.CheckFileExists = $true
        if ($Candidates.Count) { $dialog.InitialDirectory = $Candidates[0] }
        if ($dialog.ShowDialog() -ne [Windows.Forms.DialogResult]::OK) {
            throw 'Installation cancelled: AIMP.exe was not selected.'
        }
        if ([IO.Path]::GetFileName($dialog.FileName) -ne 'AIMP.exe') {
            throw 'The selected file is not AIMP.exe. Select AIMP.exe to continue.'
        }
        Split-Path -Parent $dialog.FileName
    } finally {
        $dialog.Dispose()
    }
}
function Resolve-AimpFolder {
    param([string]$RequestedPath)
    if ($RequestedPath) {
        if (-not (Test-Path -LiteralPath (Join-Path $RequestedPath 'AIMP.exe') -PathType Leaf)) {
            throw "AIMP.exe not found in $RequestedPath. Select the folder containing AIMP.exe to continue."
        }
        return [IO.Path]::GetFullPath($RequestedPath)
    }
    $candidates = @(Get-AimpInstallations)
    if ($candidates.Count -eq 1) { return $candidates[0] }
    Select-AimpFolder -Candidates $candidates
}
function Enable-AirPlayDsp {
    param([Parameter(Mandatory)][string]$AimpIni)
    if (-not ('AimpAirPlayIni' -as [type])) {
        Add-Type @'
using System.Runtime.InteropServices;
public static class AimpAirPlayIni {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool WritePrivateProfileStringW(string section, string key, string value, string path);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern uint GetPrivateProfileIntW(string section, string key, int fallback, string path);
}
'@
    }
    $AimpIni = [IO.Path]::GetFullPath($AimpIni)
    $section = 'AIMPSoundOut.DSP'
    # AIMP's DSP identifier for DSP_AIRPLAY.DLL.
    $key = '00000000065F87DC'
    if ([AimpAirPlayIni]::GetPrivateProfileIntW($section, $key, 0, $AimpIni) -eq 1) { return }
    if (Test-Path -LiteralPath $AimpIni) {
        Copy-Item -LiteralPath $AimpIni -Destination ($AimpIni + '.airplay.previous') -Force
    } else {
        New-Item -ItemType Directory -Path (Split-Path -Parent $AimpIni) -Force | Out-Null
        [IO.File]::WriteAllText($AimpIni, '', [Text.Encoding]::Unicode)
    }
    if (-not [AimpAirPlayIni]::WritePrivateProfileStringW($section, $key, '1', $AimpIni) -or
        [AimpAirPlayIni]::GetPrivateProfileIntW($section, $key, 0, $AimpIni) -ne 1) {
        throw "Could not enable AirPlay PCM in $AimpIni. Check write access to the AIMP profile and run Install.ps1 again."
    }
}
function Test-AirPlayFirewall {
    param([string]$ReceiverIP, [string]$AimpPath, [string]$ProfilePath)
    $sharedStatus = $sharedMessage = ''
    try {
        if (-not $ReceiverIP -and (Test-Path -LiteralPath $ProfilePath)) {
            $section = ''
            foreach ($line in [IO.File]::ReadAllLines($ProfilePath)) {
                if ($line -match '^\s*\[([^]]+)\]\s*$') { $section = $Matches[1]; continue }
                if ($section -eq 'LastReceiver' -and $line -match '^\s*Host\s*=\s*(.*?)\s*$') { $ReceiverIP = $Matches[1]; break }
            }
        }
        $address = $null
        if (-not [Net.IPAddress]::TryParse($ReceiverIP, [ref]$address) -or
            $address.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
            $sharedStatus = 'NoReceiver'
            $sharedMessage = 'Firewall: receiver IPv4 is not known yet. Select a receiver, then run Install.ps1 -CheckFirewallOnly again.'
        } else {
            $ReceiverIP = $address.ToString()
            $profiles = @(Get-NetFirewallProfile -PolicyStore ActiveStore)
            if (-not $profiles.Count) { throw 'No firewall profiles returned.' }
            if (-not @($profiles | Where-Object { $_.Enabled -eq $true }).Count) {
                $sharedStatus = 'Disabled'
                $sharedMessage = 'Firewall: Windows Firewall is disabled for all profiles; no inbound rule is required by it.'
            }
        }
    } catch {
        $sharedStatus = 'Unavailable'
        $sharedMessage = 'Firewall check unavailable: ' + $_.Exception.Message
    }
    $checks = foreach ($protocol in @('UDP', 'TCP')) {
        $status, $message = $sharedStatus, $sharedMessage
        if (-not $status) {
            try {
                $rule = Get-NetFirewallRule -PolicyStore ActiveStore -Name ('AimpAirPlay' + $protocol + '-' + $ReceiverIP) -ErrorAction SilentlyContinue
                $status = 'Missing'
                if ($rule) {
                    $application = $rule | Get-NetFirewallApplicationFilter
                    $port = $rule | Get-NetFirewallPortFilter
                    $scope = $rule | Get-NetFirewallAddressFilter
                    $program = [Environment]::ExpandEnvironmentVariables($application.Program)
                    $valid = $rule.Enabled -eq 'True' -and $rule.Direction -eq 'Inbound' -and $rule.Action -eq 'Allow' -and
                             $rule.Profile -eq 'Any' -and [IO.Path]::GetFullPath($program) -eq [IO.Path]::GetFullPath($AimpPath) -and
                             $port.Protocol -in @($protocol, $(if ($protocol -eq 'UDP') { '17' } else { '6' })) -and $port.LocalPort -eq 'Any' -and $port.RemotePort -eq 'Any' -and
                             $scope.LocalAddress -eq 'Any' -and @($scope.RemoteAddress).Count -eq 1 -and $scope.RemoteAddress -eq $ReceiverIP
                    $status = if ($valid) { 'Ready' } else { 'Mismatch' }
                }
                $message = switch ($status) {
                    'Missing' { "Firewall: AIMP $protocol rule for $ReceiverIP is missing. Run Install.ps1 -AllowFirewall to add it." }
                    'Ready' { "Firewall: inbound $protocol rule for AIMP and $ReceiverIP is enabled." }
                    'Mismatch' { "Firewall: the AIMP $protocol rule has different settings. Check it in Windows Firewall." }
                }
            } catch {
                $status = 'Unavailable'
                $message = 'Firewall check unavailable: ' + $_.Exception.Message
            }
        }
        [pscustomobject]@{ Status = $status; ReceiverIP = $ReceiverIP; Message = $message; Protocol = $protocol }
    }
    $first = $checks | Where-Object { $_.Status -notin @('Ready', 'Disabled') } | Select-Object -First 1
    if (-not $first) { $first = $checks[0] }
    $message = $first.Message
    if ($first.Status -eq 'Ready') { $message = "Firewall: inbound UDP and TCP rules for AIMP and $($first.ReceiverIP) are enabled." }
    [pscustomobject]@{ Status = $first.Status; ReceiverIP = $first.ReceiverIP; Message = $message; Checks = @($checks) }
}
if ($CheckFirewallOnly) {
    if ($AllowFirewall) { throw 'CheckFirewallOnly cannot modify firewall rules. Use AllowFirewall during installation.' }
    $AimpPath = Resolve-AimpFolder -RequestedPath $AimpPath
    return Test-AirPlayFirewall -ReceiverIP $ReceiverIP -AimpPath (Join-Path $AimpPath 'AIMP.exe') -ProfilePath $ProfilePath
}
if (Get-Process AIMP -ErrorAction SilentlyContinue) { throw 'Close AIMP before installing.' }
$AimpPath = Resolve-AimpFolder -RequestedPath $AimpPath
$BuildPath = [IO.Path]::GetFullPath($BuildPath)
$pluginFiles = @{
    aimp_airplay = @('aimp_airplay.dll', 'libcrypto-3-x64.dll', 'libssl-3-x64.dll')
    dsp_airplay = @('dsp_airplay.dll')
}
foreach ($files in $pluginFiles.Values) {
    foreach ($file in $files) {
        $source = Join-Path $BuildPath $file
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Missing build file: $source. Build the project before installing."
        }
    }
}
foreach ($plugin in @('aimp_airplay', 'dsp_airplay')) {
    $destination = Join-Path (Join-Path $AimpPath 'Plugins') $plugin
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    foreach ($file in $pluginFiles[$plugin]) {
        $source = Join-Path $BuildPath $file
        $target = Join-Path $destination $file
        if (Test-Path -LiteralPath $target) { Copy-Item -LiteralPath $target -Destination ($target + '.previous') -Force }
        Copy-Item -LiteralPath $source -Destination $target -Force
        if ((Get-FileHash -LiteralPath $source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) {
            throw "Installed file verification failed: $target. Run Install.ps1 again."
        }
    }
}
Enable-AirPlayDsp -AimpIni (Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($ProfilePath))) 'AIMP.ini')
Write-Host 'Installed. AirPlay PCM enabled.'
$result = Test-AirPlayFirewall -ReceiverIP $ReceiverIP -AimpPath (Join-Path $AimpPath 'AIMP.exe') -ProfilePath $ProfilePath
if ($AllowFirewall -and @($result.Checks | Where-Object { $_.Status -eq 'Missing' }).Count) {
    foreach ($check in $result.Checks | Where-Object { $_.Status -eq 'Missing' }) {
        New-NetFirewallRule -Name ('AimpAirPlay' + $check.Protocol + '-' + $check.ReceiverIP) -DisplayName ('AIMP AirPlay ' + $check.Protocol + ' from ' + $check.ReceiverIP) -Direction Inbound -Action Allow -Program (Join-Path $AimpPath 'AIMP.exe') -Protocol $check.Protocol -RemoteAddress $check.ReceiverIP -Profile Any | Out-Null
    }
    $result = Test-AirPlayFirewall -ReceiverIP $result.ReceiverIP -AimpPath (Join-Path $AimpPath 'AIMP.exe') -ProfilePath $ProfilePath
}
Write-Host $result.Message
