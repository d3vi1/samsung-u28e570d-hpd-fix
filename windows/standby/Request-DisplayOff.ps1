#requires -Version 5.1
[CmdletBinding(SupportsShouldProcess=$true)]
param([switch]$RequestDisplayOff, [string]$CaptureDirectory)
$ErrorActionPreference = 'Stop'
if (-not $RequestDisplayOff) { throw 'This separate active operation requires -RequestDisplayOff.' }
if (-not $PSCmdlet.ShouldProcess('Windows displays', 'Request display-off through WM_SYSCOMMAND / SC_MONITORPOWER')) { return }
if ($CaptureDirectory) {
    & (Join-Path $PSScriptRoot 'Standby-Capture.ps1') -Action Mark -Directory $CaptureDirectory -Text 'Operator requested Windows display-off (SC_MONITORPOWER=2).'
}
if (-not ('WindowsStandbyExplicit.DisplayPower' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace WindowsStandbyExplicit {
    public static class DisplayPower {
        [DllImport("user32.dll", SetLastError=true)]
        private static extern IntPtr SendMessageTimeout(IntPtr hwnd, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
        public static void RequestOff() {
            UIntPtr result;
            if (SendMessageTimeout(new IntPtr(0xffff), 0x0112, new UIntPtr(0xf170), new IntPtr(2), 2, 2000, out result) == IntPtr.Zero)
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
    }
}
'@
}
[WindowsStandbyExplicit.DisplayPower]::RequestOff()
[pscustomobject]@{utc=[DateTimeOffset]::UtcNow.ToString('O');request='display-off';physicalStandbyConfirmed=$false}
