$ErrorActionPreference = 'Continue'
Get-Process WINWORD -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2

if ($args.Count -gt 0) {
    $docx = Get-Item -LiteralPath $args[0]
} else {
    $dir = "D:\wb(cn)\BMS\docs\" + [char]0x8BF4 + [char]0x660E + [char]0x4E66
    $docx = Get-ChildItem -LiteralPath $dir -Filter "*.docx" |
            Where-Object { $_.Name -notlike "~`$*" -and $_.Name -notlike "_*" } |
            Select-Object -First 1
}
"src=" + $docx.Name

$word = $null; $doc = $null
try {
    $word = New-Object -ComObject Word.Application
    $word.Visible = $false
    $word.DisplayAlerts = 0
    $doc = $word.Documents.Open($docx.FullName, $false, $false)
    $doc.Repaginate()

    $rows = @()
    foreach ($p in $doc.Paragraphs) {
        # ★ 用大纲级别而非样式名：本脚本是 UTF-8，PS 5.1 按 GBK 读会把中文样式名读成乱码
        $lvl = 0
        try { $lvl = [int]$p.OutlineLevel } catch { $lvl = 0 }
        if ($lvl -ge 1 -and $lvl -le 2) {
            $t = $p.Range.Text
            $t = $t -replace "[\r\n\x07\x0B]", ""
            $t = $t.Trim()
            if ($t.Length -gt 0) {
                $pg = $p.Range.Information(3)   # wdActiveEndPageNumber
                $rows += [PSCustomObject]@{ text = $t; page = [int]$pg }
            }
        }
    }
    "headings=" + $rows.Count
    $json = $rows | ConvertTo-Json -Depth 3
    [System.IO.File]::WriteAllText("D:\wb(cn)\BMS\docs\manual\pages.json", $json, (New-Object System.Text.UTF8Encoding($false)))
    "pages-written"
} catch {
    "ERR: " + $_.Exception.Message
} finally {
    if ($doc) { try { $doc.Close(0) } catch {} }
    if ($word) { try { $word.Quit() } catch {} }
}
