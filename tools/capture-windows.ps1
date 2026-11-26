[CmdletBinding()]
param(
    [string] $Package = (Join-Path $PSScriptRoot '../downloads/Under20-Windows.zip'),
    [Parameter(Mandatory)] [string] $Output
)
$ErrorActionPreference = 'Stop'
$ffmpeg = (Get-Command ffmpeg.exe -ErrorAction Stop).Source
$ffprobe = (Get-Command ffprobe.exe -ErrorAction Stop).Source
$work = Join-Path ([IO.Path]::GetTempPath()) ('under20-demo-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -Type Directory -Path $work
$null = New-Item -Type Directory -Path $Output -Force
$Output = (Resolve-Path $Output).Path

# Capture only the actual app's client area. No desktop, taskbar, or other windows.
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Text;
public static class Under20DemoWindow {
    [StructLayout(LayoutKind.Sequential)] struct Rect { public int left, top, right, bottom; }
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr w, out Rect r);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr w, IntPtr dc, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetDlgItem(IntPtr w, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)]
    static extern IntPtr SendMessageTimeout(IntPtr w, uint msg, UIntPtr capacity, StringBuilder text, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    public static string Detail(IntPtr window) {
        var value = new StringBuilder(2048);
        UIntPtr copied;
        SendMessageTimeout(GetDlgItem(window,107),13,(UIntPtr)2048,value,2,1000,out copied);
        return value.ToString();
    }
    public static void Capture(IntPtr window, string path) {
        Rect r;
        if (!GetClientRect(window,out r) || r.right < 300 || r.bottom < 250) throw new Exception("Window dimensions unavailable.");
        using (var image = new Bitmap(r.right,r.bottom,PixelFormat.Format24bppRgb)) {
            using (var graphics = Graphics.FromImage(image)) {
                var dc = graphics.GetHdc();
                try { if (!PrintWindow(window,dc,3)) throw new Exception("Window capture failed."); }
                finally { graphics.ReleaseHdc(dc); }
            }
            var colors = new HashSet<int>();
            for (int y=0;y<image.Height;y+=4) for (int x=0;x<image.Width;x+=4)
                colors.Add(image.GetPixel(x,y).ToArgb());
            if (colors.Count < 20) throw new Exception("The Windows desktop returned a blank frame.");
            image.Save(path,ImageFormat.Png);
        }
    }
}
'@
$null = [Under20DemoWindow]::SetProcessDPIAware()

function Invoke-Media([string[]] $MediaArgs) {
    & $ffmpeg -hide_banner -nostdin -v error -y @MediaArgs
    if ($LASTEXITCODE -ne 0) { throw "FFmpeg exited with $LASTEXITCODE." }
}
function Get-AppWindow($Process) {
    $end = [DateTime]::UtcNow.AddSeconds(25)
    do {
        $Process.Refresh()
        if ($Process.HasExited) { throw "Under20.exe exited with $($Process.ExitCode)." }
        if ($Process.MainWindowHandle -ne [IntPtr]::Zero) { return $Process.MainWindowHandle }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $end)
    throw 'The desktop app did not open a window.'
}
function Get-MediaInfo([string] $Path) {
    $report = & $ffprobe -v error -show_streams -show_format -of json $Path
    if ($LASTEXITCODE -ne 0) { throw 'FFprobe failed.' }
    return ($report -join "`n" | ConvertFrom-Json)
}

$app = $null
try {
    $appDir = Join-Path $work 'application'
    Expand-Archive -LiteralPath $Package -DestinationPath $appDir
    $exe = Join-Path $appDir 'Under20.exe'
    $video = Join-Path $work 'Motion-study.mkv'
    $audio = Join-Path $work 'Studio-tone.wav'
    $chime = Join-Path $work 'Chime.m4a'
    Invoke-Media @('-f','lavfi','-i','testsrc2=size=960x540:rate=30:duration=12','-f','lavfi','-i','sine=frequency=392:sample_rate=48000:duration=12','-vf','noise=alls=7:allf=t+u:all_seed=73','-c:v','ffv1','-threads:v','3','-c:a','pcm_s16le','-ac','2',$video)
    Invoke-Media @('-f','lavfi','-i','sine=frequency=330:sample_rate=48000:duration=125','-ac','2','-c:a','pcm_s16le',$audio)
    Invoke-Media @('-f','lavfi','-i','sine=frequency=660:sample_rate=48000:duration=1','-c:a','aac',$chime)
    foreach ($p in @($video,$audio)) {
        if ((Get-Item $p).Length -lt 20000000) { throw "Test input is too small: $p" }
    }
    $app = Start-Process $exe -WorkingDirectory $work -PassThru
    $window = Get-AppWindow $app
    Start-Sleep -Milliseconds 500
    [Under20DemoWindow]::Capture($window,(Join-Path $Output 'ready.png'))
    $null = $app.CloseMainWindow()
    if (!$app.WaitForExit(5000)) { throw 'The empty demo window did not close.' }

    # Explorer uses these same file arguments when files are dropped on the executable.
    $inputs = @($video,$audio,$chime)
    $quoted = ($inputs | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $app = Start-Process $exe -ArgumentList $quoted -WorkingDirectory $work -PassThru
    $window = Get-AppWindow $app
    $mp4 = Join-Path $work 'Motion-study-under-20MB.mp4'
    $flac = Join-Path $work 'Studio-tone-under-20MB.flac'
    $seen = @{}
    $end = [DateTime]::UtcNow.AddMinutes(5)
    do {
        if ($app.HasExited) { throw 'The compressor exited while processing the batch.' }
        $detail = [Under20DemoWindow]::Detail($window)
        foreach ($item in @(@('Analyzing video','analyzing'),@('Encoding video','encoding'),@('Decoding the finished copy','verifying'))) {
            if ($detail.Contains($item[0]) -and !$seen.ContainsKey($item[1])) {
                [Under20DemoWindow]::Capture($window,(Join-Path $Output ($item[1] + '.png')))
                $seen[$item[1]] = $true
            }
        }
        if ((Test-Path -LiteralPath $mp4) -and (Test-Path -LiteralPath $flac)) { break }
        if ($detail.Contains('failed') -or $detail.Contains('Could not')) { throw $detail }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $end)
    if (!(Test-Path $mp4) -or !(Test-Path $flac)) { throw "Incomplete demo batch. Selected row: $detail" }
    Start-Sleep -Milliseconds 900
    [Under20DemoWindow]::Capture($window,(Join-Path $Output 'saved.png'))
    if (Get-ChildItem $work -Filter 'Chime-under-20MB*') { throw 'The small input should have been skipped.' }

    $results = foreach ($pair in @(@($video,$mp4),@($audio,$flac))) {
        $inputFile = Get-Item $pair[0]; $outputFile = Get-Item $pair[1]
        if ($outputFile.Length -ge 20000000 -or $outputFile.Length -le 0) { throw 'Output size outside the cap.' }
        Invoke-Media @('-xerror','-i',$outputFile.FullName,'-map','0:V?','-map','0:a?','-f','null','-')
        $origin = Get-MediaInfo $inputFile.FullName
        $result = Get-MediaInfo $outputFile.FullName
        if ([Math]::Abs([double]$origin.format.duration - [double]$result.format.duration) -gt 0.12) { throw 'Duration mismatch.' }
        foreach ($type in @('video','audio')) {
            $a = @($origin.streams | Where-Object codec_type -eq $type)
            $b = @($result.streams | Where-Object codec_type -eq $type)
            if ($a.Count -ne $b.Count) { throw 'Track count mismatch.' }
            if ($a.Count -and $type -eq 'audio' -and $a[0].channels -ne $b[0].channels) { throw 'Channel count mismatch.' }
            if ($a.Count -and $type -eq 'video' -and ($a[0].width -ne $b[0].width -or $a[0].height -ne $b[0].height)) { throw 'Dimensions changed.' }
        }
        [ordered]@{input=$inputFile.Name; input_bytes=$inputFile.Length; output=$outputFile.Name; output_bytes=$outputFile.Length; duration_seconds=$result.format.duration}
    }
    $results += [ordered]@{input='Chime.m4a'; input_bytes=(Get-Item $chime).Length; result='skipped'}
    $results | ConvertTo-Json | Set-Content (Join-Path $Output 'results.json') -Encoding UTF8
    $results | ConvertTo-Json
}
finally {
    if ($app -and !$app.HasExited) {
        $null = $app.CloseMainWindow()
        if (!$app.WaitForExit(5000)) { $app.Kill(); $app.WaitForExit() }
    }
    Remove-Item -LiteralPath $work -Recurse -Force
}
