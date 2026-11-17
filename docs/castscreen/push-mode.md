# 原生推投屏控制协议（V1.0.0）

员工端 Sunshine 捕获并编码；大屏端 Moonlight 解码。员工点击投屏后，通过控制通道让大屏反向连接员工电脑。视频不经过注册服务器。

## 进程

- `sunshine.exe sender-ui`：员工界面、发现、投屏请求；Sunshine 媒体宿主负责配对和串流。
- `Moonlight.exe standby`：无界面待机。
- `Moonlight.exe standby --ui` 或普通 GUI：界面、托盘、待机服务，共用一个 ComputerManager。
- `Moonlight.exe stream <UUID> Desktop`：待机服务启动的串流子进程，不再按可能重复的 IP 选择陈旧主机记录。

## 发现与控制

员工向 UDP 47998 广播 `LANPROJ/1 DISCOVER`。大屏单播返回名称、TCP 控制端口、状态及版本。广播受阻时可手动填大屏 IP，发送端 GET /state；重复输入已有 IP 也会重新探测。

| 请求 | 作用 |
|---|---|
| GET /state | 待机服务状态、名称、完整性、托盘状态 |
| POST /cast | `{ "host": "员工主机名", "sender": "显示名", "address": "员工IP" }` |
| POST /stop | 停止串流或取消当前配对请求 |

默认控制 TCP 47999、发现 UDP 47998。receiver.conf 支持 name、port、discovery_port、force_video_args；环境端口覆盖沿用现有实现。

## 自动配对

1. 每次 cast 都通过员工实际地址刷新主机，取得本次 HTTP 发现的准确 UUID。
2. 用客户端证书派生 PIN，执行原有四阶段配对握手。Sunshine 根据已授权证书或配置的企业网段自动批准；两者均不匹配仍等待原版 PIN。
3. 新主机成功结果进入 PairingHost 分支；缓存证书只用于选择验证路径，不能当作服务端已授权的证据。
4. 握手成功后，等待共享主机表写入 QSettings，重新读取并核对该 UUID 的服务器证书 DER，再启动串流子进程。
5. 任一步失败/超时返回 412 auto_pair_failed，不拿旧记录继续开流。活动请求或未退出的配对 worker 返回 409 busy。取消/超时后的晚到成功不启动视频。

成功 HTTP 200 表示握手、落盘和串流进程启动完成；不保证已收到首帧。当前 streaming 状态表示子进程在运行，现场画面仍需验收。

默认不强制视频参数，串流读取用户的 Moonlight 画质设置。只有 receiver.conf 中 force_video_args=1 才附加固定 1080p60、20Mbps、全屏参数。

## 信任边界

Sunshine enterprise_auto_approve_lan 默认开启，enterprise_auto_approve_cidr 默认172.16.0.0/12。该模式仅用于用户明确授权的可信企业内网；不应暴露公网。派生 PIN 可由公开证书计算，不能称为秘密或直接宣称等价 mTLS。需要更严格的控制时关闭网段自动批准并使用证书白名单；独立协议安全审计仍未完成。

控制通道目前没有独立的设备身份鉴权、会话所有权或审计服务，网段自动批准也不能证明员工身份。

## 网络

| 端口 | 方向/作用 |
|---|---|
| UDP 47998 | 员工发发现，大屏回包 |
| TCP 47999 | 员工调用大屏控制 |
| TCP 47984/47989 | 大屏连接 Sunshine 配对与信令 |
| TCP 47990 | Sunshine Web UI |
| GameStream 相关 TCP/UDP | 原版媒体与控制，沿用 Sunshine/Moonlight 端口需求 |

两个安装侧的防火墙规则和实际网络均会影响连通。可手动拉流不证明待机控制端口可达。删除主机不应关闭待机服务；回归修复与验收见 receiver-regressions.md。

早期 .NET 注册服务保留作历史组件，当前推模式不依赖它。接收端设置迁入原生 QML 页仍为待办。
