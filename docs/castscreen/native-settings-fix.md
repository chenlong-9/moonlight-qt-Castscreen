# 原生设置修复版 2026-11-17

## 现场原因及修复

1. 用户实际启动 artifacts/NativeSettings-20261117/receiver/Moonlight.exe，进程 Low IL。09:30 日志显示免 PIN 配对成功、随后 host registry flush 失败 integrity=low；不是退回旧版。该文件 hash 847E5F38FA6BDAE9E5067FAF848AFC1D0EEC7DA16F9CED11B10018D21DD0128B 与上一轮部署一致。
2. 名称 TextField 直接绑定 roomName，而 shared NOTIFY 同时用于服务状态/防火墙，状态通知重置输入。现改为页面加载初始化编辑草稿，保存成功才回填，不受后台刷新覆盖。
3. 防火墙 helper 在 QCoreApplication 创建前调用 applicationFilePath，路径为空；netsh 收到 program= 空参数而失败。helper 分支现创建 QCoreApplication，程序规则验证非空路径，TCP/UDP 端口规则不再包含 program 参数。
4. 检测拒绝访问时，规则状态显示未知，避免把未检测误当缺失；确认进程为 Low 时明确提示安装至普通用户目录，不继续调用受限的放行助手。

## 本机运行

当前 %LOCALAPPDATA%\Programs\LANProjection\Receiver-NativeSettings-Fix\Moonlight.exe，桌面快捷方式“会议室接收端-修复版”。保留用户设置/配对及“测试会议室”名称。不要运行工作区 receiver 下的 exe。

修复 exe SHA256：079F90A8B2286DA048F73018856A5F3EEDA6B39EA5835BA42A5038BB15C9E254。

测试包包含 install-receiver-test.cmd；先退出旧接收端，然后从包根目录运行该脚本，复制普通用户目录再启动。包不含现场 receiver.conf 或日志。发送端无需更新。

## 验证

- 原生构建 exit0，QML 解析及 diff --check 通过。
- 设置/持久化隔离测试 24 项通过 exit0。
- 实际运行 /state：medium、idle、tray=true，名称“测试会议室”。实际页面防火墙检测“已放行”。
- 本轮未实际点击 UAC 修改规则，也未重新验收双机投屏；helper 修复已编译，修改系统规则路径仍需现场验证。未清理历史主机或证书，未更改配对协议。
