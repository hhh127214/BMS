$ErrorActionPreference = 'Continue'
Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

$src = $args[0]
$tmp = "C:\Windows\Temp\manual_v115c.pdf"
if (Test-Path $tmp) { Remove-Item $tmp -Force }

$word = $null; $doc = $null
try {
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $doc = $word.Documents.Open($src, $false, $false)
    "pages=" + $doc.ComputeStatistics(2)
    # ★ 目标路径必须是纯 ASCII —— 中文路径会让 Word 静默失败
    $doc.SaveAs2($tmp, 17)
    "saved"
} catch {
    "ERR: " + $_.Exception.Message
} finally {
    if ($doc) { try { $doc.Close(0) } catch {} }
    if ($word) { try { $word.Quit() } catch {} }
}
"exists=" + (Test-Path $tmp)
