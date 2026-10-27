# shunti IME (Windows 版) を入れる・外す (install.bat・uninstall.bat から呼ぶ)。
#   入れる: powershell -ExecutionPolicy Bypass -File install.ps1
#   外す:   powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall
# このスクリプトと同じフォルダの shunti_ime_x64.dll・shunti_ime_x86.dll・kkc_lex.bin・kkc_model.bin を
# C:\Program Files\shunti IME に写し、64 ビットと 32 ビットの両方を登録して、使う人のキーボードの一覧に足す。
# ファイルの写しと登録は管理者の権限で (UAC で頼む)、キーボードの一覧はその人の設定なので権限なしで行う。
# 使っているアプリが DLL を開いたままでも入れ直せるように、古いファイルは名前を変えて残し、次の再起動で消す。
param([switch]$Uninstall, [switch]$Elevated)
$ErrorActionPreference = 'Stop'

$Dest = Join-Path $env:ProgramFiles 'shunti IME'
$Files = @('shunti_ime_x64.dll', 'shunti_ime_x86.dll', 'shunti_settings.exe', 'kkc_lex.bin', 'kkc_model.bin', 'LICENSE.txt')
# 軽量・高精度の変換モデル (設定で選ぶ)。無ければ標準 (kkc_model.bin) だけで動く
$Optional = @('kkc_model_light.bin', 'kkc_variants.tsv')   # kkc_variants.tsv = M5 の書き方の対の表
# スタートメニューの「shunti IME の設定」(全員分)
$Shortcut = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\shunti IME の設定.lnk'
$Is64 = [Environment]::Is64BitOperatingSystem
$Reg64 = Join-Path $env:windir 'System32\regsvr32.exe'
$Reg32 = if ($Is64) { Join-Path $env:windir 'SysWOW64\regsvr32.exe' } else { $Reg64 }
# 入力の方法の印 (言語 0411 = 日本語、テキストサービスの CLSID、プロファイルの GUID)
$Log = Join-Path $env:TEMP 'shunti-ime-install.log'   # 管理者の側で失敗した理由 (こちらの窓に出す)
$Tip = '0411:{3CA2ED9D-36D7-4E5E-92FB-5A702C39A46B}{80F9DCBC-6C43-4043-B004-C6275415630E}'

Add-Type -Namespace Shunti -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
public static extern bool MoveFileEx(string from, string to, int flags);
[DllImport("input.dll", CharSet = CharSet.Unicode)]
public static extern bool InstallLayoutOrTip(string list, int flags);
'@

# ---- 管理者の権限で行う部分: ファイルの写しと登録

# 使用中なら名前を変えて、次の再起動で消す
function Remove-OrRetire([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    try { Remove-Item -LiteralPath $path -Force; return } catch {}
    $old = "$path.old-" + [Guid]::NewGuid().ToString('N').Substring(0, 8)
    Rename-Item -LiteralPath $path -NewName (Split-Path $old -Leaf) -Force
    [void][Shunti.Native]::MoveFileEx($old, $null, 4)   # MOVEFILE_DELAY_UNTIL_REBOOT
}

# 登録に使う regsvr32 と DLL の組 (32 ビット版の Windows では 32 ビットだけ)
function Get-Pairs {
    $pairs = New-Object System.Collections.Generic.List[object]
    $pairs.Add([pscustomobject]@{ Reg = $Reg32; Dll = 'shunti_ime_x86.dll' })
    if ($Is64) { $pairs.Add([pscustomobject]@{ Reg = $Reg64; Dll = 'shunti_ime_x64.dll' }) }
    return $pairs
}

function Unregister-All {
    foreach ($pair in (Get-Pairs)) {
        $dll = Join-Path $Dest $pair.Dll
        if (Test-Path -LiteralPath $dll) { Start-Process $pair.Reg -ArgumentList @('/u', '/s', "`"$dll`"") -Wait }
    }
}

function Invoke-AdminPart {
    if ($Uninstall) {
        Unregister-All
        Remove-Item -LiteralPath $Shortcut -Force -ErrorAction SilentlyContinue
        if (Test-Path -LiteralPath $Dest) {
            Get-ChildItem -LiteralPath $Dest -File | ForEach-Object { Remove-OrRetire $_.FullName }
            try { Remove-Item -LiteralPath $Dest -Force -ErrorAction SilentlyContinue } catch {}
        }
        return
    }
    $src = $PSScriptRoot
    foreach ($f in $Files) {
        if (-not (Test-Path -LiteralPath (Join-Path $src $f))) { throw "$f が見つかりません ($src)。zip を全部展開してから実行してください" }
    }
    Unregister-All
    # 開いている設定の画面を閉じる (設定は変えるたびに保存しているので失うものは無い)。残すと、名前を変えた古い版の画面が
    # 開いたままになり、切り替えが保存されないことがあった (2026-10-27)
    Get-Process -Name 'shunti_settings' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $Dest | Out-Null
    Get-ChildItem -LiteralPath $Dest -Filter '*.old-*' -File -ErrorAction SilentlyContinue | ForEach-Object {
        try { Remove-Item -LiteralPath $_.FullName -Force } catch {}
    }
    # キー配列の表 (layout_*.tsv) もあれば全部
    $Layouts = @(Get-ChildItem -LiteralPath $src -Filter 'layout_*.tsv' -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    foreach ($f in $Files + $Layouts + ($Optional | Where-Object { Test-Path -LiteralPath (Join-Path $src $_) })) {
        $to = Join-Path $Dest $f
        Remove-OrRetire $to
        Copy-Item -LiteralPath (Join-Path $src $f) -Destination $to -Force
    }
    foreach ($pair in (Get-Pairs)) {
        $p = Start-Process $pair.Reg -ArgumentList @('/s', "`"$(Join-Path $Dest $pair.Dll)`"") -Wait -PassThru
        if ($p.ExitCode -ne 0) { throw "登録に失敗しました ($($pair.Dll): $($p.ExitCode))" }
    }
    try {
        $lnk = (New-Object -ComObject WScript.Shell).CreateShortcut($Shortcut)
        $lnk.TargetPath = Join-Path $Dest 'shunti_settings.exe'
        $lnk.WorkingDirectory = $Dest
        $lnk.Description = 'shunti IME の設定とユーザー辞書'
        $lnk.Save()
    } catch {}   # スタートメニューに置けなくても IME は使える (タスクバーのアイコンの右クリックからも開ける)
}

if ($Elevated) {
    try {
        Invoke-AdminPart
        exit 0
    } catch {
        try { "$_" | Out-File -LiteralPath $Log -Encoding utf8 } catch {}
        exit 1
    }
}

# ---- 使う人の権限で行う部分

function Invoke-Elevated {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($isAdmin) { Invoke-AdminPart; return }
    Remove-Item -LiteralPath $Log -Force -ErrorAction SilentlyContinue
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated')
    if ($Uninstall) { $a += '-Uninstall' }
    try {
        $p = Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList $a -Wait -PassThru
    } catch {
        throw '管理者の許可 (UAC) が得られませんでした'
    }
    if ($p.ExitCode -ne 0) {
        $why = if (Test-Path -LiteralPath $Log) { (Get-Content -LiteralPath $Log -Raw -Encoding utf8).Trim() } else { '' }
        throw "ファイルの写しか登録に失敗しました $why"
    }
}

try {
    if ($Uninstall) {
        [void][Shunti.Native]::InstallLayoutOrTip($Tip, 1)   # キーボードの一覧から外す (ILOT_UNINSTALL)
        Invoke-Elevated
        Write-Host ''
        Write-Host 'shunti IME を外しました。'
        Write-Host '使っていたアプリを閉じるか、パソコンを再起動すると、残りのファイルも消えます。'
        Write-Host '(学習とユーザー辞書は %APPDATA%\shunti IME に残っています。要らなければそのフォルダを消してください)'
    } else {
        Write-Host 'shunti IME を入れています... (管理者の許可を求める画面が出たら「はい」を押してください)'
        Invoke-Elevated
        $ok = [Shunti.Native]::InstallLayoutOrTip($Tip, 0)   # キーボードの一覧に足す
        Write-Host ''
        Write-Host 'shunti IME を入れました。' -ForegroundColor Green
        if (-not $ok) {
            Write-Host 'キーボードの一覧には自動で足せませんでした。設定 → 時刻と言語 → 言語と地域 → 日本語 の「…」→'
            Write-Host '言語のオプション → キーボードの追加 で「shunti IME」を選んでください。'
        }
        Write-Host 'Windows キー + スペース で「shunti IME」に切り替えて使ってください。'
        Write-Host '(すでに開いていたアプリでは、開き直すと使えるようになります)'
    }
    exit 0
} catch {
    Write-Host ''
    Write-Host "失敗しました: $_" -ForegroundColor Red
    exit 1
}
