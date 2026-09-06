<#
.SYNOPSIS
  把 live2d_mate 的 crash-*.log 裡的 `module+RVA` 解析成函式名與 檔名:行號。

.DESCRIPTION
  出貨版只帶 /PDBSTRIPPED 產的精簡 pdb，所以使用者寄回來的 crash 檔
  **有函式名、沒有行號**。這支腳本吃那份 log 加上 release 另外掛的完整 pdb，
  把行號補回去。

  設計要點（改動前請先讀）：

  1. **零外部相依。** 符號化走 System32 的 dbghelp.dll（每台 Windows 都有），
     用 P/Invoke 呼叫，不需要 Windows SDK 的 Debugging Tools，也不需要 LLVM。
     **完全不會啟動 live2d_mate.exe** —— DbgHelp 只是讀檔。

  2. **一定要先核對 PDB GUID/age。** 拿錯 pdb 的後果不是「解不出來」，而是
     「格式完全正確、每一行都指向別的函式」，而且從輸出上完全看不出來。
     所以預設在核對失敗時直接拒絕執行（-Force 可跳過，但你得自己知道在做什麼）。
     核對的來源是 **exe 的 debug directory**（RSDS 記錄），不是 pdb 內部 ——
     PDB 的 MSF 格式要自己剖析太重，而 exe 的 debug directory 只有幾十行。
     核完之後把配對交給 DbgHelp：它本來就會拒絕 GUID/age 對不上的 pdb，
     所以「解得出符號」本身就是第二道驗證。

     這一步也順帶區分了兩種都會「沒有行號」但成因完全不同的情況：
     **pdb 拿錯了** vs **拿到的是精簡 pdb**。少了核對，這兩者長得一模一樣。

  3. **回傳位址要先減 1 再查。** `#01` 以後每一格都是「call 的下一道指令」的位址；
     那個 call 若剛好是某一行（或某個函式）的最後一道指令，不減 1 會歸到下一行、
     甚至下一個函式，整份符號化靜靜偏移一格。
     **但 SEH 路徑的 `#00` 是 faulting instruction 本身，不能減。**
     判斷依據：reason 那一行帶 `(detail)` 括號的碼都是 handler 自己合成的
     （見 src/platform/crash_handler.h 的已知限制），那條路走的是
     CaptureStackBackTrace，`#00` 也是回傳位址；沒有括號才是 SEH。

  4. **查詢用的基底由 DbgHelp 自己決定**（`SymLoadModuleExW` 的 base 傳 0，用它的回傳值），
     不是 log 裡那個真實 base —— 真實 base 有機會撞到本行程已載入的模組，
     而 DbgHelp 只把 base 當記帳用。

     **踩過的坑：`SYMOPT_DEFERRED_LOADS` 加上 `SizeOfDll = 0` 是致命組合。**
     模組會被登記成大小 0，之後每一個位址都落在模組範圍外，符號**連載都不會載**，
     `SymFromAddrW` 一律回 false。症狀是每一格都印 `(no symbol)`，看起來像 pdb 有問題
     或 pdb 被剝過，實際上跟 pdb 一點關係都沒有。實測：延後載入與 size=0 只要拿掉
     任一個就正常。這支腳本是一次性工具，延後載入沒有任何好處，所以直接不開。

  已知天花板：非 exe 的 frame（Qt、系統 DLL）解不了 —— 要它們的 pdb 才行，
  不在這支腳本的範圍。crash 檔在第二階段會補上那些 frame 的模組名，
  所以至少看得出「死在誰家」。

.PARAMETER Log
  crash-*.log 的路徑。可以給多個（支援萬用字元）。

.PARAMETER Pdb
  完整 pdb 的路徑，或放著它的目錄。

.PARAMETER Exe
  對應那次建置的 live2d_mate.exe。省略時會在 pdb 旁邊找同名的 .exe。
  **必須是產生那份 crash 檔的同一次建置**，GUID/age 核對就是在核對這件事。

.PARAMETER InPlace
  在 log 檔尾端追加一段 `--- resolved ---`，而不是印到主控台。

.PARAMETER Force
  GUID/age 對不上時仍然繼續。**輸出會不可信**，只在你明確知道原因時使用。

.EXAMPLE
  .\Resolve-CrashLog.ps1 -Log "$env:APPDATA\live2d_mate\logs\crash-20260831-150230-18676.log" `
                         -Pdb .\build\rel\live2d_mate.pdb

.EXAMPLE
  # 一次處理整個資料夾，結果直接寫回各自的 log
  .\Resolve-CrashLog.ps1 -Log "$env:APPDATA\live2d_mate\logs\crash-*.log" -Pdb .\symbols -InPlace
#>
[CmdletBinding()]
param(
  [Parameter(Mandatory, Position = 0)] [string[]] $Log,
  [Parameter(Mandatory)] [string] $Pdb,
  [string] $Exe,
  [switch] $InPlace,
  [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ([IntPtr]::Size -ne 8) {
  throw "這支腳本只支援 64 位元 PowerShell（本專案是 msvc2022_64）。"
}


# ---------------------------------------------------------------------------
# DbgHelp P/Invoke
#
# 刻意用「自己配置緩衝 + 手寫欄位位移」而不是宣告 struct：SYMBOL_INFOW 與
# IMAGEHLP_LINEW64 都是尾端變長的結構，交給 marshaller 排版時 SizeOfStruct
# 會因為尾端填補而算錯，而那個欄位錯了 DbgHelp 只會回失敗，不會告訴你為什麼。
# ---------------------------------------------------------------------------
if (-not ('L2M.DbgHelp' -as [type])) {
  Add-Type -Namespace L2M -Name DbgHelp -MemberDefinition @'
[DllImport("dbghelp.dll", SetLastError = true)]
public static extern uint SymSetOptions(uint options);

[DllImport("dbghelp.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool SymInitializeW(IntPtr hProcess, string userSearchPath, bool invade);

[DllImport("dbghelp.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern ulong SymLoadModuleExW(IntPtr hProcess, IntPtr hFile, string imageName,
                                            string moduleName, ulong baseOfDll, uint dllSize,
                                            IntPtr data, uint flags);

[DllImport("dbghelp.dll", SetLastError = true)]
public static extern bool SymFromAddrW(IntPtr hProcess, ulong address, out ulong displacement,
                                       IntPtr symbolInfo);

[DllImport("dbghelp.dll", SetLastError = true)]
public static extern bool SymGetLineFromAddrW64(IntPtr hProcess, ulong address,
                                                out uint displacement, IntPtr line);

[DllImport("dbghelp.dll", SetLastError = true)]
public static extern bool SymCleanup(IntPtr hProcess);
'@
}

# SYMBOL_INFOW：SizeOfStruct 是「不含 Name 陣列內容」的標頭大小，x64 上是 88。
# 這個常數不能用 Marshal.SizeOf 推算（見上面的註解）
$script:SymbolInfoHeader = 88
$script:SymbolNameChars = 1024

# ---------------------------------------------------------------------------
# PE 讀取：從 exe 的 debug directory 取出 RSDS 的 GUID 與 age
# ---------------------------------------------------------------------------
function Get-PeCodeViewInfo {
  param([Parameter(Mandatory)] [string] $Path)

  $bytes = [System.IO.File]::ReadAllBytes($Path)
  if ($bytes.Length -lt 0x40) { throw "$Path 太小，不是有效的 PE 檔。" }
  if ($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "$Path 沒有 MZ 簽章。" }

  $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
  if ([BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550) {  # "PE\0\0"
    throw "$Path 沒有 PE 簽章。"
  }

  $coff = $peOffset + 4
  $numberOfSections = [BitConverter]::ToUInt16($bytes, $coff + 2)
  $sizeOfOptional = [BitConverter]::ToUInt16($bytes, $coff + 16)
  $optional = $coff + 20

  # 0x20B = PE32+（64 位元）。資料目錄在 optional header 的固定位移之後
  $magic = [BitConverter]::ToUInt16($bytes, $optional)
  $dataDirOffset = if ($magic -eq 0x20B) { $optional + 112 } else { $optional + 96 }

  # 資料目錄 index 6 = IMAGE_DIRECTORY_ENTRY_DEBUG，每項 8 位元組（RVA + Size）
  $debugRva = [BitConverter]::ToUInt32($bytes, $dataDirOffset + 6 * 8)
  $debugSize = [BitConverter]::ToUInt32($bytes, $dataDirOffset + 6 * 8 + 4)
  if ($debugRva -eq 0 -or $debugSize -eq 0) {
    throw "$Path 沒有 debug directory —— 它可能不是帶符號資訊的建置。"
  }

  # RVA -> 檔案位移，要走 section 表
  $sectionTable = $optional + $sizeOfOptional
  $debugFileOffset = $null
  for ($i = 0; $i -lt $numberOfSections; $i++) {
    $s = $sectionTable + $i * 40
    $va = [BitConverter]::ToUInt32($bytes, $s + 12)
    $rawSize = [BitConverter]::ToUInt32($bytes, $s + 16)
    $rawPtr = [BitConverter]::ToUInt32($bytes, $s + 20)
    if ($debugRva -ge $va -and $debugRva -lt ($va + $rawSize)) {
      $debugFileOffset = $rawPtr + ($debugRva - $va)
      break
    }
  }
  if ($null -eq $debugFileOffset) { throw "$Path 的 debug directory RVA 不在任何 section 裡。" }

  # 逐一掃 IMAGE_DEBUG_DIRECTORY（28 位元組），找 Type == 2（CODEVIEW）
  $count = [int]($debugSize / 28)
  for ($i = 0; $i -lt $count; $i++) {
    $d = $debugFileOffset + $i * 28
    $type = [BitConverter]::ToUInt32($bytes, $d + 12)
    if ($type -ne 2) { continue }
    $cvOffset = [BitConverter]::ToUInt32($bytes, $d + 24)   # PointerToRawData
    if ([BitConverter]::ToUInt32($bytes, $cvOffset) -ne 0x53445352) { continue }  # "RSDS"

    # RSDS：簽章(4) + GUID(16) + Age(4) + pdb 路徑（NUL 結尾）
    $guidBytes = New-Object byte[] 16
    [Array]::Copy($bytes, $cvOffset + 4, $guidBytes, 0, 16)
    $guid = [guid]::new($guidBytes)
    $age = [BitConverter]::ToUInt32($bytes, $cvOffset + 20)

    $end = $cvOffset + 24
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0) { $end++ }
    $pdbPath = [System.Text.Encoding]::UTF8.GetString($bytes, $cvOffset + 24, $end - ($cvOffset + 24))

    return [pscustomobject]@{ Guid = $guid; Age = $age; PdbPath = $pdbPath }
  }
  throw "$Path 的 debug directory 裡沒有 RSDS（CODEVIEW）記錄。"
}

# ---------------------------------------------------------------------------
# crash 檔剖析
# ---------------------------------------------------------------------------
function Read-CrashLog {
  param([Parameter(Mandatory)] [string] $Path)

  $lines = Get-Content -LiteralPath $Path -Encoding utf8
  $info = [ordered]@{
    Path = $Path; Module = $null; Guid = $null; Age = $null
    Synthesized = $false; Frames = @()
  }

  foreach ($line in $lines) {
    if ($line -match '^\s*image\s*:\s*(\S+)\s+base=0x([0-9A-Fa-f]+)') {
      $info.Module = $Matches[1]
      continue
    }
    if ($line -match '^\s*pdb\s*:\s*([0-9A-Fa-f-]{36})\s+age=(\d+)') {
      $info.Guid = [guid]$Matches[1]
      $info.Age = [uint32]$Matches[2]
      continue
    }
    if ($line -match '^\s*reason\s*:\s*') {
      # 帶 (detail) 括號的 reason 碼都是 handler 自己合成的 —— 那條路走
      # CaptureStackBackTrace，#00 也是回傳位址（見檔頭第 3 點）
      $info.Synthesized = $line -match '\('
      continue
    }
    # `stack :` 段的 frame。只收得出 module+RVA 的那些；純絕對位址是
    # Qt／系統 DLL，沒有它們的 pdb 就解不了
    if ($line -match '^\s*#(\d+)\s+(\S+?)\+0x([0-9A-Fa-f]+)\s*$') {
      $info.Frames += [pscustomobject]@{
        Index = [int]$Matches[1]; Module = $Matches[2]; Rva = [uint64]"0x$($Matches[3])"
      }
    }
  }
  if (-not $info.Module) { throw "$Path 裡找不到 `image :` 那一行，不像是 live2d_mate 的 crash 檔。" }
  [pscustomobject]$info
}

# ---------------------------------------------------------------------------
# 符號化
# ---------------------------------------------------------------------------
function Resolve-Frames {
  param(
    [Parameter(Mandatory)] $Crash,
    [Parameter(Mandatory)] [string] $ExePath,
    [Parameter(Mandatory)] [string] $SearchPath
  )

  # 0x02 UNDNAME（還原修飾名）| 0x10 LOAD_LINES。
  # **刻意不開 0x04 DEFERRED_LOADS** —— 見檔頭第 4 點那個坑
  [void][L2M.DbgHelp]::SymSetOptions(0x02 -bor 0x10)

  # 用一個假的行程 handle：DbgHelp 只拿它當 key，不會去碰真的行程
  $h = [IntPtr]::new(0x4C324D)
  if (-not [L2M.DbgHelp]::SymInitializeW($h, $SearchPath, $false)) {
    throw "SymInitializeW 失敗（Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())）。"
  }

  $symBufSize = $script:SymbolInfoHeader + $script:SymbolNameChars * 2
  $symBuf = [Runtime.InteropServices.Marshal]::AllocHGlobal($symBufSize)
  $lineBuf = [Runtime.InteropServices.Marshal]::AllocHGlobal(40)
  $zeroSym = New-Object byte[] $symBufSize
  try {
    # base 傳 0 讓 DbgHelp 用 PE 的 preferred ImageBase，回傳值就是它實際用的基底
    $base = [L2M.DbgHelp]::SymLoadModuleExW($h, [IntPtr]::Zero, $ExePath, $null,
                                            0, 0, [IntPtr]::Zero, 0)
    if ($base -eq 0) {
      throw "SymLoadModuleExW 失敗（Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())）。"
    }

    $results = @()
    foreach ($f in $Crash.Frames) {
      # 回傳位址減 1 再查；SEH 路徑的 #00 是 faulting instruction，不減（檔頭第 3 點）
      $isFaulting = (-not $Crash.Synthesized) -and ($f.Index -eq 0)
      $lookup = $base + $f.Rva - $(if ($isFaulting) { 0 } else { 1 })

      $name = $null; $lineText = $null

      # **整個緩衝都要清零，不能只寫 SizeOfStruct。** SYMBOL_INFOW 那 88 位元組
      # 標頭裡的其他欄位 DbgHelp 是會讀的，留著 AllocHGlobal 的垃圾會讓
      # SymFromAddrW 靜靜失敗——症狀是每一格都 (no symbol)，看起來像 pdb 有問題
      [Runtime.InteropServices.Marshal]::Copy($zeroSym, 0, $symBuf, $zeroSym.Length)
      [Runtime.InteropServices.Marshal]::WriteInt32($symBuf, 0, $script:SymbolInfoHeader)
      [Runtime.InteropServices.Marshal]::WriteInt32($symBuf, 80, $script:SymbolNameChars - 1)
      $disp = [uint64]0
      if ([L2M.DbgHelp]::SymFromAddrW($h, $lookup, [ref]$disp, $symBuf)) {
        $nameLen = [Runtime.InteropServices.Marshal]::ReadInt32($symBuf, 76)
        if ($nameLen -gt 0) {
          $raw = [Runtime.InteropServices.Marshal]::PtrToStringUni(
                   [IntPtr]::Add($symBuf, 84), $nameLen)
          # 印的是「原始位址」與函式起點的差，跟行程內第二階段的輸出一致。
          # 查詢時減掉的那 1 要加回來，否則每個 displacement 都會少 1
          $realDisp = $disp + $(if ($isFaulting) { 0 } else { 1 })
          $name = '{0}+0x{1:X}' -f $raw, $realDisp
        }
      }

      [Runtime.InteropServices.Marshal]::WriteInt64($lineBuf, 0, 0)
      [Runtime.InteropServices.Marshal]::WriteInt64($lineBuf, 8, 0)
      [Runtime.InteropServices.Marshal]::WriteInt64($lineBuf, 16, 0)
      [Runtime.InteropServices.Marshal]::WriteInt64($lineBuf, 24, 0)
      [Runtime.InteropServices.Marshal]::WriteInt64($lineBuf, 32, 0)
      [Runtime.InteropServices.Marshal]::WriteInt32($lineBuf, 0, 40)
      $lineDisp = [uint32]0
      if ([L2M.DbgHelp]::SymGetLineFromAddrW64($h, $lookup, [ref]$lineDisp, $lineBuf)) {
        $number = [Runtime.InteropServices.Marshal]::ReadInt32($lineBuf, 16)
        $filePtr = [Runtime.InteropServices.Marshal]::ReadIntPtr($lineBuf, 24)
        if ($filePtr -ne [IntPtr]::Zero) {
          $file = [Runtime.InteropServices.Marshal]::PtrToStringUni($filePtr)
          # 只印檔名：完整路徑會洩漏建置機器的目錄結構，對照原始碼時檔名就夠
          $lineText = '{0}:{1}' -f (Split-Path -Leaf $file), $number
        }
      }

      $results += [pscustomobject]@{
        Index = $f.Index
        Frame = '{0}+0x{1:X8}' -f $f.Module, $f.Rva
        Symbol = $name
        Line = $lineText
      }
    }
    $results
  } finally {
    [Runtime.InteropServices.Marshal]::FreeHGlobal($symBuf)
    [Runtime.InteropServices.Marshal]::FreeHGlobal($lineBuf)
    [void][L2M.DbgHelp]::SymCleanup($h)
  }
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
$pdbItem = Get-Item -LiteralPath $Pdb
$searchPath = if ($pdbItem.PSIsContainer) { $pdbItem.FullName } else { $pdbItem.DirectoryName }

if (-not $Exe) {
  $candidate = Join-Path $searchPath 'live2d_mate.exe'
  if (-not (Test-Path -LiteralPath $candidate)) {
    throw "沒有給 -Exe，也沒在 $searchPath 找到 live2d_mate.exe。GUID/age 核對需要那次建置的 exe。"
  }
  $Exe = $candidate
}
$exePath = (Get-Item -LiteralPath $Exe).FullName
$cv = Get-PeCodeViewInfo -Path $exePath
Write-Verbose "exe RSDS: $($cv.Guid) age=$($cv.Age)"

$logFiles = @()
foreach ($pattern in $Log) { $logFiles += Get-Item -Path $pattern }
if (-not $logFiles) { throw "找不到任何 log 檔。" }

$failed = 0

foreach ($file in $logFiles) {
  $crash = Read-CrashLog -Path $file.FullName

  if ($crash.Guid -and -not ($crash.Guid -eq $cv.Guid -and $crash.Age -eq $cv.Age)) {
    $msg = @"
$($file.Name) 與這份 exe/pdb 不是同一次建置：
  log 記的 : $($crash.Guid) age=$($crash.Age)
  exe 記的 : $($cv.Guid) age=$($cv.Age)
硬解出來的行號會格式正確但全部指向錯的地方。請改用產生這份 log 的那次建置的符號檔。
"@
    if (-not $Force) {
      # -ErrorAction Continue 是必要的：檔頭設了 $ErrorActionPreference = 'Stop'，
      # 不覆寫的話 Write-Error 會終止整個腳本，下面那個 continue 永遠跑不到 ——
      # 症狀是「批次處理一整個資料夾時，第一份對不上的 log 就把後面全部帶走」
      Write-Error $msg -ErrorAction Continue
      $failed++
      continue
    }
    Write-Warning "$msg（-Force 已指定，繼續。輸出不可信。）"
  }

  if (-not $crash.Frames) {
    Write-Warning "$($file.Name)：`stack :` 段裡沒有任何 $($crash.Module)+0x… 的 frame，沒東西可解。"
    continue
  }

  $resolved = Resolve-Frames -Crash $crash -ExePath $exePath -SearchPath $searchPath

  $withLines = @($resolved | Where-Object { $_.Line }).Count
  if ($withLines -eq 0) {
    Write-Warning @"
$($file.Name)：解得出函式名但一個行號都沒有。
GUID/age 核對已通過，所以最可能的原因是你手上這份是 /PDBSTRIPPED 產的**精簡** pdb
（只有 public symbols，沒有行號）。行號要用 release 另外掛的完整 pdb。
"@
  }

  $out = New-Object System.Collections.Generic.List[string]
  $out.Add('--- resolved ---')
  $out.Add("  pdb  : $($cv.Guid) age=$($cv.Age)")
  foreach ($r in $resolved) {
    $parts = @('  #{0:D2} {1}' -f $r.Index, $r.Frame)
    if ($r.Symbol) { $parts += $r.Symbol } else { $parts += '(no symbol)' }
    if ($r.Line) { $parts += $r.Line }
    $out.Add(($parts -join '  '))
  }

  if ($InPlace) {
    Add-Content -LiteralPath $file.FullName -Value '' -Encoding utf8
    Add-Content -LiteralPath $file.FullName -Value $out -Encoding utf8
    Write-Host "已追加 --- resolved --- 到 $($file.Name)（$withLines/$($resolved.Count) 格有行號）"
  } else {
    Write-Host "=== $($file.Name) ==="
    $out | ForEach-Object { Write-Host $_ }
  }
}

if ($failed -gt 0) {
  Write-Host ""
  Write-Host "$failed 份 log 因為符號檔對不上而跳過。" -ForegroundColor Yellow
  exit 1
}
