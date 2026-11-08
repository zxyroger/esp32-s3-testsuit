# 把 ESP32 摄像头的画面发布到公网，手机浏览器直接打开下面显示的地址即可。
#
# 用法（在 D:\s3demo 下）：
#   .\view.ps1                  用默认摄像头地址 192.168.0.105
#   .\view.ps1 -CamIp 192.168.0.120
#
# 说明：
#   - 走到是 pinggy.io 的 SSH 反向隧道，只依赖系统自带的 ssh.exe，不用装任何东西。
#   - 免费版一条隧道 60 分钟，到期重跑本脚本即可（域名每次都会变）。
#   - 关掉这个 PowerShell 窗口，隧道就断了，网页随即打不开 —— 这是正常的。
#   - 曾经的 cloudflared 快速隧道在当前网络下会被 Cloudflare 判为
#     Error 1000（DNS points to prohibited IP），试过 5 个域名全部被拒，
#     所以这里默认走 pinggy。详见 docs/摄像头公网访问.md。
param([string]$CamIp = "192.168.0.105")

Write-Host ""
Write-Host "== ESP32 摄像头 · 公网发布 ==" -ForegroundColor Cyan

# ---- 1) 先确认板子在线，避免建了个空隧道还以为是隧道坏了 ----
Write-Host "[1/2] 检查摄像头 $CamIp ... " -NoNewline
$stats = & curl.exe -s --noproxy '*' -m 6 "http://$CamIp/stats" 2>$null
if (-not $stats) {
    Write-Host "连不上" -ForegroundColor Red
    Write-Host "      确认板子已上电并加入 WiFi；串口发 'wifi ip' 可以看到当前地址" -ForegroundColor DarkGray
    Write-Host ""
    Read-Host "按回车退出"
    exit 1
}
Write-Host "在线" -ForegroundColor Green
Write-Host "      $stats" -ForegroundColor DarkGray
Write-Host ""

# ---- 2) 建隧道 ----
Write-Host "[2/2] 建立公网隧道 ..." -ForegroundColor Cyan
Write-Host "      域名大约 3 秒后出现在下面那行。60 分钟有效，关窗口即断开。" -ForegroundColor DarkGray
Write-Host ""

$ssh = Join-Path $env:SystemRoot "System32\OpenSSH\ssh.exe"
if (-not (Test-Path $ssh)) { $ssh = "ssh" }

& $ssh -p 443 -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL `
       -o ServerAliveInterval=20 -o ExitOnForwardFailure=yes `
       -R "0:${CamIp}:80" a.pinggy.io
