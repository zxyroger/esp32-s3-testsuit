' 静默启动「串口 <-> MQTT」桥接（无窗口、不随启动它的窗口退出）
'
' 作用：把 COM14 上板子的串口输出发到 MQTT 的 <prefix>/serial/tx，
'       同时把网页发到 <prefix>/serial/rx 的命令写进串口。
'       这样手机浏览器就能远程敲板子的命令，不需要公网 IP、不需要隧道。
'
' 用法：双击本文件；或由开机自启项调用。
' 停止：任务管理器结束 pythonw.exe，或先 release 串口再刷机。
Option Explicit
Dim sh, pyw, script, workdir

pyw = "C:\Espressif\tools\python\v6.0.2\venv\Scripts\pythonw.exe"
workdir = "D:\s3demo"
script = workdir & "\bridge_serial_mqtt.py"

Set sh = CreateObject("WScript.Shell")
sh.CurrentDirectory = workdir
' 0 = 隐藏窗口, False = 不等待（wscript 立刻退出，pythonw 独立存活）
sh.Run """" & pyw & """ """ & script & """ COM14", 0, False
