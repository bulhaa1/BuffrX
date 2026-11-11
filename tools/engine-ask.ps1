# Sends one protocol command to a running engine and prints its reply: a probe for scripts and for
# checking the native interface without hands, e.g.
#   powershell -File tools/engine-ask.ps1 status.get
#   powershell -File tools/engine-ask.ps1 ui.capture '{"view":"settings:audio","file":"C:/temp/audio.png"}'
param(
  [Parameter(Mandatory = $true)][string]$Command,
  [string]$Arguments = '{}',
  [int]$TimeoutMs = 20000,
  # The arguments as a file. A JSON argument with quotes in it does not survive the Windows
  # command line, so a path with a space or a quote in it has to come in this way.
  [string]$ArgumentsFile = ''
)

$ErrorActionPreference = 'Stop'
$code = @'
using System; using System.IO; using System.IO.Pipes; using System.Text; using System.Threading.Tasks;
public static class EngineAsk {
    static void Write(Stream s, string payload) {
        var body = Encoding.UTF8.GetBytes(payload);
        s.Write(BitConverter.GetBytes((uint)body.Length), 0, 4); s.Write(body, 0, body.Length); s.Flush();
    }
    static string Read(Stream s) {
        while (true) {
            var head = new byte[4]; int n = 0;
            while (n < 4) { int r = s.Read(head, n, 4 - n); if (r <= 0) throw new IOException("the engine closed the pipe"); n += r; }
            uint length = BitConverter.ToUInt32(head, 0); var body = new byte[length]; n = 0;
            while (n < length) { int r = s.Read(body, n, (int)length - n); if (r <= 0) throw new IOException("the engine closed the pipe"); n += r; }
            var text = Encoding.UTF8.GetString(body);
            if (text.Contains("\"event\"")) continue;  // events arrive between replies
            return text;
        }
    }
    public static string Ask(string pipeName, string payload, int ms) {
        using (var pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous)) {
            pipe.Connect(5000);
            Write(pipe, payload);
            var reply = Task.Run(() => Read(pipe));
            if (!reply.Wait(ms)) return "{\"ok\":false,\"error\":\"no reply in time\"}";
            return reply.Result;
        }
    }
}
'@
Add-Type -TypeDefinition $code
if ($ArgumentsFile) { $Arguments = ([System.IO.File]::ReadAllText($ArgumentsFile)).Trim() }
$payload = '{"id":1,"cmd":"' + $Command + '","args":' + $Arguments + '}'
# BUFFRX_PIPE lets a probe talk to a throwaway engine (tools/repro.ps1) instead of the user's.
$pipeName = if ($env:BUFFRX_PIPE) { $env:BUFFRX_PIPE } else { 'buffrx' }
[EngineAsk]::Ask($pipeName, $payload, $TimeoutMs)
