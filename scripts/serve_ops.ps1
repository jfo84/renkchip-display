# serve_ops.ps1 — bare-bones HTTP file server for the stick's DLNA player.
# Raw TcpListener (no HttpListener URL ACL needed). Serves ops.mp4 for any GET.
$dir  = "$PSScriptRoot"
$file = Join-Path $dir "ops.mp4"
$port = 8000
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Any, $port)
$listener.Start()
Write-Output "serving $file on :$port"
while ($true) {
  $client = $listener.AcceptTcpClient()
  try {
    $stream = $client.GetStream()
    $reader = [System.IO.StreamReader]::new($stream)
    $null = $reader.ReadLine()  # request line; we serve the one file regardless
    while (($line = $reader.ReadLine()) -ne $null -and $line -ne "") {}
    $bytes = [System.IO.File]::ReadAllBytes($file)
    $head = "HTTP/1.1 200 OK`r`nContent-Type: video/mp4`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n"
    $hb = [System.Text.Encoding]::ASCII.GetBytes($head)
    $stream.Write($hb, 0, $hb.Length)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()
  } catch { Write-Output "err: $_" } finally { $client.Close() }
}
