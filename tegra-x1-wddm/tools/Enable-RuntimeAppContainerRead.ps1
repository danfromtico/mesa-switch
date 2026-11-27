# SPDX-License-Identifier: MIT
# Test runtime binaries live outside DriverStore. Packaged desktop components
# need both ordinary-user and AppContainer read/execute access to these files.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
$root=(Get-Item -LiteralPath $Directory).FullName
$runtime=@('TegraGpuD3D.dll','libvulkan_nouveau.dll','vulkan-1.dll')
foreach($name in $runtime){
    if(!(Test-Path -LiteralPath (Join-Path $root $name) -PathType Leaf)){
        throw ('Expected runtime file: '+$name)
    }
}
$before=(Get-Acl -LiteralPath $root).Sddl
# Only this artifact directory and its children; no parent or system ACL change.
& icacls.exe $root /grant '*S-1-15-2-1:(OI)(CI)(RX)' | Out-Null
if($LASTEXITCODE -ne 0){throw 'AppContainer read/execute grant failed'}
$files=@()
foreach($name in $runtime){
    $path=Join-Path $root $name
    $acl=Get-Acl -LiteralPath $path
    $allowed=@($acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]) |
        Where-Object {$_.IdentityReference.Value -eq 'S-1-15-2-1' -and
            $_.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
            ($_.FileSystemRights -band [Security.AccessControl.FileSystemRights]::ReadAndExecute) -eq
                [Security.AccessControl.FileSystemRights]::ReadAndExecute})
    if(!$allowed.Count){throw ('Read/execute ACL missing: '+$name)}
    $files+=@{Name=$name;Sddl=$acl.Sddl}
}
@{Directory=$root;PackageSid='S-1-15-2-1';Access='ReadAndExecute';
    BeforeSddl=$before;AfterSddl=(Get-Acl -LiteralPath $root).Sddl;Files=$files} |
    ConvertTo-Json -Depth 4
