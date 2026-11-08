#!/usr/bin/env bash
# 把 ESP32 摄像头的画面发布到公网，断了自动重连。
#
# 为什么要这个脚本：
#   pinggy 免费隧道固定 60 分钟就被服务端掐断（日志里是 Time exceeded），域名随即失效。
#   本脚本一直循环：建隧道 -> 抓域名写进 viewer_url.txt -> 等它断 -> 立刻重建。
#   窗口开着公网入口就一直可用，只是域名每小时换一次。
#
# 用法（在 D:\s3demo 下，用 Git Bash）：
#   ./view-watch.sh                 默认摄像头 192.168.0.105
#   ./view-watch.sh 192.168.0.120   指定其它地址
#
# 为什么是 bash 不是 PowerShell：
#   PowerShell（5.1）里 `& ssh ... | ForEach-Object` 收不到 ssh 的 stdout，
#   Start-Job 会卡在 ssh 的块缓冲上，Start-Process 又会被本机
#   http_proxy / HTTP_PROXY 这对大小写重复的环境变量搞崩。
#   bash 的 `ssh > 文件` 是实测最稳的，3~6 秒就能拿到域名。
set -u

CAM_IP="${1:-192.168.0.105}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
URL_FILE="$HERE/viewer_url.txt"
OUT="$HERE/tunnel_current.log"
RETRY=3

SSH_OPTS=(-p 443
          -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null
          -o ServerAliveInterval=20
          -o ServerAliveCountMax=3
          -o ExitOnForwardFailure=yes
          -o LogLevel=ERROR)

echo "== ESP32 摄像头 · 公网发布（自动重连） =="
echo "   摄像头 $CAM_IP ，隧道断开后 ${RETRY}s 内自动重建"
echo "   按 Ctrl+C 停止"
echo

round=0
while true; do
    round=$((round + 1))

    # 每轮先确认板子还在，避免隧道建成空壳还以为是隧道坏了
    if ! curl -s --noproxy '*' -m 6 "http://$CAM_IP/stats" > /dev/null 2>&1; then
        echo "[$round] 摄像头连不上，${RETRY}s 后重试（串口发 wifi ip 查地址）"
        sleep "$RETRY"
        continue
    fi

    : > "$OUT"
    ssh "${SSH_OPTS[@]}" -R "0:${CAM_IP}:80" a.pinggy.io > "$OUT" 2>&1 &
    pid=$!

    url=""
    for _ in $(seq 1 60); do
        sleep 0.5
        url=$(grep -o 'https://[a-z0-9-]*\.run\.pinggy-free\.link' "$OUT" 2>/dev/null | head -1)
        [ -n "$url" ] && break
        kill -0 "$pid" 2>/dev/null || break
    done

    if [ -n "$url" ]; then
        sub=$(printf '%s' "$url" | sed -E 's#https://([a-z0-9-]+)\.run\.pinggy-free\.link#\1#')
        now=$(date '+%Y-%m-%d %H:%M')
        exp=$(date -d '+60 min' '+%Y-%m-%d %H:%M')

        cat > "$URL_FILE" <<EOF
ESP32 摄像头公网入口
当前: $url
备用: https://$sub.free.pinggy.net
局域网: http://$CAM_IP/
生成: $now
过期: $exp（pinggy 免费隧道固定 60 分钟）
说明: view-watch.sh 正在运行，到期会换新域名并自动更新本文件
EOF
        echo
        echo "  [当前地址] $url"
        echo "  有效期到 ${exp#* }，断线后自动重建（域名会变，看 viewer_url.txt）"
        echo
    else
        echo "[$round] 这一轮没拿到域名，重试"
        kill -9 "$pid" 2>/dev/null
        sleep "$RETRY"
        continue
    fi

    # 阻塞等隧道断开（60 分钟到期或网络抖动）
    wait "$pid" 2>/dev/null
    echo "[$round] 隧道已断开（$(date '+%H:%M:%S')），${RETRY}s 后重建"
    sleep "$RETRY"
done
