# PixInsight 中文 · 图形化安装器
# 双击运行，选择 PixInsight 安装目录后一键安装
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Definition

# ---------- 随包文件清单 ----------
$files = @(
    @{ src = Join-Path $here 'bin\pi_zh.dll';        name = 'pi_zh.dll' },
    @{ src = Join-Path $here 'bin\pi_zh_loader.exe'; name = 'pi_zh_loader.exe' },
    @{ src = Join-Path $here 'zh_cn.tsv';            name = 'zh_cn.tsv' },
    @{ src = Join-Path $here '使用说明.txt';          name = '使用说明.txt' }
)

# ---------- 校验随包文件 ----------
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f.src)) {
        [System.Windows.Forms.MessageBox]::Show(
            "安装包里缺少 $($f.name)，请重新解压完整压缩包。",
            'PixInsight 中文', 'OK', 'Error') | Out-Null
        exit 1
    }
}

# ---------- 构建界面 ----------
$form = New-Object System.Windows.Forms.Form
$form.Text = 'PixInsight 中文 · 安装'
$form.Size = New-Object System.Drawing.Size(560, 320)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 9)

# 标题
$lblTitle = New-Object System.Windows.Forms.Label
$lblTitle.Text = 'PixInsight 简体中文汉化 安装程序'
$lblTitle.Font = New-Object System.Drawing.Font('Microsoft YaHei UI', 13, [System.Drawing.FontStyle]::Bold)
$lblTitle.Location = New-Object System.Drawing.Point(20, 18)
$lblTitle.AutoSize = $true
$form.Controls.Add($lblTitle)

$lblSub = New-Object System.Windows.Forms.Label
$lblSub.Text = '不修改 PixInsight 任何原版文件，删除 zh_cn 目录即还原。'
$lblSub.ForeColor = [System.Drawing.Color]::Gray
$lblSub.Location = New-Object System.Drawing.Point(22, 52)
$lblSub.AutoSize = $true
$form.Controls.Add($lblSub)

# 目录输入框
$lblDir = New-Object System.Windows.Forms.Label
$lblDir.Text = 'PixInsight 安装目录：'
$lblDir.Location = New-Object System.Drawing.Point(22, 90)
$lblDir.AutoSize = $true
$form.Controls.Add($lblDir)

$txtDir = New-Object System.Windows.Forms.TextBox
$txtDir.Location = New-Object System.Drawing.Point(22, 112)
$txtDir.Size = New-Object System.Drawing.Size(410, 24)
$form.Controls.Add($txtDir)

$btnBrowse = New-Object System.Windows.Forms.Button
$btnBrowse.Text = '浏览…'
$btnBrowse.Location = New-Object System.Drawing.Point(440, 110)
$btnBrowse.Size = New-Object System.Drawing.Size(88, 28)
$form.Controls.Add($btnBrowse)

# 提示文字
$lblHint = New-Object System.Windows.Forms.Label
$lblHint.Text = '选择 PixInsight 安装目录（含 bin\PixInsight.exe），或直接选择 bin 目录'
$lblHint.ForeColor = [System.Drawing.Color]::Gray
$lblHint.Location = New-Object System.Drawing.Point(22, 144)
$lblHint.AutoSize = $true
$form.Controls.Add($lblHint)

# 进度条
$progress = New-Object System.Windows.Forms.ProgressBar
$progress.Location = New-Object System.Drawing.Point(22, 180)
$progress.Size = New-Object System.Drawing.Size(506, 20)
$progress.Style = 'Continuous'
$progress.Minimum = 0
$progress.Maximum = 100
$progress.Value = 0
$form.Controls.Add($progress)

$lblStatus = New-Object System.Windows.Forms.Label
$lblStatus.Text = ''
$lblStatus.Location = New-Object System.Drawing.Point(22, 210)
$lblStatus.Size = New-Object System.Drawing.Size(506, 20)
$lblStatus.AutoSize = $false
$form.Controls.Add($lblStatus)

# 按钮
$btnInstall = New-Object System.Windows.Forms.Button
$btnInstall.Text = '安装'
$btnInstall.Location = New-Object System.Drawing.Point(360, 246)
$btnInstall.Size = New-Object System.Drawing.Size(80, 30)
$form.Controls.Add($btnInstall)

$btnCancel = New-Object System.Windows.Forms.Button
$btnCancel.Text = '取消'
$btnCancel.Location = New-Object System.Drawing.Point(448, 246)
$btnCancel.Size = New-Object System.Drawing.Size(80, 30)
$form.Controls.Add($btnCancel)

# ---------- 事件 ----------
$btnBrowse.Add_Click({
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = '请选择 PixInsight 安装目录，或直接选择 bin 目录'
    if ($dlg.ShowDialog() -eq 'OK') {
        $txtDir.Text = $dlg.SelectedPath
    }
})

$btnCancel.Add_Click({ $form.Close() })

$btnInstall.Add_Click({
    $sel = $txtDir.Text.Trim().Trim('"').Trim("'").TrimEnd('\')
    if ($sel -eq '') {
        [System.Windows.Forms.MessageBox]::Show('请先选择 PixInsight 安装目录。', 'PixInsight 中文', 'OK', 'Warning') | Out-Null
        return
    }

    # 自动识别：兼容三种选法
    # ① 选安装根目录（含 bin\PixInsight.exe） ② 选 bin 目录（含 PixInsight.exe） ③ 直接选到含 PixInsight.exe 的任意一层
    $root = $null
    if (Test-Path -LiteralPath (Join-Path $sel 'bin\PixInsight.exe')) {
        $root = $sel
    }
    elseif (Test-Path -LiteralPath (Join-Path $sel 'PixInsight.exe')) {
        # 选中的是 bin 或直接含主程序的目录：取上一级作为安装根目录
        $root = Split-Path -Parent $sel
    }
    if (-not $root) {
        [System.Windows.Forms.MessageBox]::Show("该目录里找不到 PixInsight.exe：`n`n$sel`n`n请选择 PixInsight 安装目录（含 bin\PixInsight.exe），也可以直接选择 bin 目录。", 'PixInsight 中文', 'OK', 'Error') | Out-Null
        return
    }
    $piexe = Join-Path $root 'bin\PixInsight.exe'
    $txtDir.Text = $root   # 回填为标准根目录，用户能看到实际安装位置

    $btnInstall.Enabled = $false
    $btnBrowse.Enabled = $false
    $btnCancel.Enabled = $false
    $lblStatus.Text = '正在安装…'
    $progress.Value = 10

    try {
        # 1. 建立 zh_cn 目录
        $dst = Join-Path $root 'zh_cn'
        if (-not (Test-Path -LiteralPath $dst)) {
            New-Item -ItemType Directory -Path $dst | Out-Null
        }
        $progress.Value = 25

        # 2. 复制文件（旧文件备份）
        $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
        foreach ($f in $files) {
            $target = Join-Path $dst $f.name
            if (Test-Path -LiteralPath $target) {
                Move-Item -LiteralPath $target -Destination "$target.bak-$stamp" -Force -ErrorAction SilentlyContinue
            }
            Copy-Item -LiteralPath $f.src -Destination $target -Force
        }
        $progress.Value = 60

        # 3. 桌面快捷方式
        $desktop = [Environment]::GetFolderPath('Desktop')
        $lnkPath = Join-Path $desktop 'PixInsight 中文.lnk'
        $ws = New-Object -ComObject WScript.Shell
        $lnk = $ws.CreateShortcut($lnkPath)
        $lnk.TargetPath       = Join-Path $dst 'pi_zh_loader.exe'
        $lnk.WorkingDirectory = $dst
        $lnk.IconLocation     = "$piexe,0"
        $lnk.Description      = '以简体中文界面启动 PixInsight（不修改原版文件）'
        $lnk.Save()
        $progress.Value = 90

        $lblStatus.Text = '安装完成'
        $progress.Value = 100

        [System.Windows.Forms.MessageBox]::Show(
            "安装完成！`n`n桌面已创建「PixInsight 中文」快捷方式，双击即可启动中文版。`n`n卸载方式：删除 $dst 文件夹即可。",
            'PixInsight 中文', 'OK', 'Information') | Out-Null
        $form.Close()
    }
    catch {
        $lblStatus.Text = '安装失败：' + $_.Exception.Message
        $btnInstall.Enabled = $true
        $btnBrowse.Enabled = $true
        $btnCancel.Enabled = $true
        [System.Windows.Forms.MessageBox]::Show('安装失败：' + $_.Exception.Message, 'PixInsight 中文', 'OK', 'Error') | Out-Null
    }
})

$form.Add_Shown({ $form.Activate() })
$form.ShowDialog() | Out-Null
