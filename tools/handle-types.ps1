# Handle counts by object type for one process, from the system handle table (NtQuerySystemInformation class 64). The type index of Thread and
# Event is learnt from this script's own handles, so no table of Windows' type numbers is needed. Dot-source it:
#   . (Join-Path $PSScriptRoot 'handle-types.ps1');  Get-HandleTypes $e.Process.Id      # {Total, Thread, Event, File, Other}
#
# Why: libobs leaves one thread handle and one event per output stop (AGENTS.md), and the plain handle count cannot say which kind grew.
if (-not ('ProbeHandles' -as [type])) {
  Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Threading;
public static class ProbeHandles {
  [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int cls, IntPtr buffer, int length, out int needed);
  [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint thread);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
  [DllImport("kernel32.dll")] static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
  // {total, thread, event, file} handle counts of the process.
  public static int[] Counts(int pid) {
    uint mine = (uint)System.Diagnostics.Process.GetCurrentProcess().Id;
    var wait = new ManualResetEvent(false);
    IntPtr eventHandle = wait.SafeWaitHandle.DangerousGetHandle();
    IntPtr threadHandle = OpenThread(0x0800, false, GetCurrentThreadId());
    IntPtr fileHandle = CreateFileW("NUL", 0x80000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero);
    int size = 8 << 20, needed;
    IntPtr buffer = Marshal.AllocHGlobal(size);
    try {
      int status;
      while ((status = NtQuerySystemInformation(64, buffer, size, out needed)) == unchecked((int)0xC0000004)) {
        Marshal.FreeHGlobal(buffer); size = Math.Max(size * 2, needed + (1 << 20)); buffer = Marshal.AllocHGlobal(size);
      }
      if (status != 0) return new int[] { -1, -1, -1, -1 };
      long count = Marshal.ReadInt64(buffer, 0);
      int eventType = -1, threadType = -1, fileType = -1;
      var perType = new Dictionary<int, int>();
      int total = 0;
      for (long i = 0; i < count; i++) {
        long at = 16 + i * 40;
        long owner = Marshal.ReadInt64(buffer, (int)(at + 8));
        long value = Marshal.ReadInt64(buffer, (int)(at + 16));
        int type = Marshal.ReadInt16(buffer, (int)(at + 30)) & 0xffff;
        if (owner == mine) {
          if (value == eventHandle.ToInt64()) eventType = type;
          if (value == threadHandle.ToInt64()) threadType = type;
          if (value == fileHandle.ToInt64()) fileType = type;
        }
        if (owner == pid) { total++; int n; perType.TryGetValue(type, out n); perType[type] = n + 1; }
      }
      Func<int, int> get = t => { int n; return t >= 0 && perType.TryGetValue(t, out n) ? n : 0; };
      return new int[] { total, get(threadType), get(eventType), get(fileType) };
    } finally { Marshal.FreeHGlobal(buffer); GC.KeepAlive(wait); }
  }
}
'@
}
function Get-HandleTypes([int]$ProcessId) {
  $c = [ProbeHandles]::Counts($ProcessId)
  [pscustomobject]@{ Total = $c[0]; Thread = $c[1]; Event = $c[2]; File = $c[3] }
}
