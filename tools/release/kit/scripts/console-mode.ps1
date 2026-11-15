# Turns QuickEdit off in the kit's own console window (issue #68). With QuickEdit on, one click in the window starts
# a text selection, and Windows then pauses every program that writes to that window until Enter or Esc: players
# saw the start, the update and collect-logs 'stuck'. The setting belongs to the window, so it stays off for the
# rest of the .bat (and ends with the window); the player's other console windows are not changed.
# The window's input is opened as CONIN$ (not the standard input, which a start may have redirected), so it works
# however PowerShell was started. No Add-Type (it runs the C# compiler and writes a DLL, which Smart App Control
# may block): the kernel32 calls are declared in memory with Reflection.Emit. Never fails the caller: no console
# window, Constrained Language mode, PowerShell 7 or any other error just leaves the window as it is ($false).
# update.ps1 carries its own copy of this function (it runs alone from a temp file): keep the two the same
# (update_test.ps1 checks it).
function Disable-KitQuickEdit {
    try {
        $name = New-Object System.Reflection.AssemblyName 'OotCoopConsoleMode'
        $asm = [System.Reflection.Emit.AssemblyBuilder]::DefineDynamicAssembly($name, 'Run')
        $type = $asm.DefineDynamicModule('OotCoopConsoleMode').DefineType('OotCoopConsoleMode', 'Public, Class')
        foreach ($m in @(@('CreateFileW', [IntPtr], @([string], [uint32], [uint32], [IntPtr], [uint32], [uint32], [IntPtr])),
                @('GetConsoleMode', [bool], @([IntPtr], [uint32].MakeByRefType())),
                @('SetConsoleMode', [bool], @([IntPtr], [uint32])),
                @('CloseHandle', [bool], @([IntPtr])))) {
            $type.DefinePInvokeMethod($m[0], 'kernel32.dll', 'Public, Static, PinvokeImpl', 'Standard', $m[1],
                [Type[]]$m[2], 'Winapi', 'Unicode').SetImplementationFlags('PreserveSig')   # (else the result is lost)
        }
        $native = $type.CreateType()
        # GENERIC_READ | GENERIC_WRITE, shared for reading and writing, OPEN_EXISTING
        $in = $native::CreateFileW('CONIN$', [uint32]3221225472, [uint32]3, [IntPtr]::Zero, [uint32]3, [uint32]0, [IntPtr]::Zero)
        if ($in -eq [IntPtr]::Zero -or $in -eq [IntPtr]-1) { return $false }
        try {
            $mode = [uint32]0
            if (-not $native::GetConsoleMode($in, [ref]$mode)) { return $false }
            # ENABLE_QUICK_EDIT_MODE (0x40) off; ENABLE_EXTENDED_FLAGS (0x80) on, without it the change is ignored.
            $new = [uint32](($mode -band 0xFFFFFFBF) -bor 0x80)
            return [bool]$native::SetConsoleMode($in, $new)
        } finally { [void]$native::CloseHandle($in) }
    } catch { return $false }
}
