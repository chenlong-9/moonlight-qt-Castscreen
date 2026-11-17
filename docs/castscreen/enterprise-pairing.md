# 企业版免 PIN 配对（V1.0.0 已实现）

## 目标

会议室大屏（Moonlight 分支）与员工电脑（Sunshine 分支）之间：

- 已授权设备之间**零操作配对**：不出现 4 位 PIN 输入提示，Sunshine 直接批准；
- 未授权设备仍走原版 PIN 流程，不会被自动放行；
- 不改变视频/编码链路，不改反向控制策略（仍不开启键鼠回传）。

## 原版协议的关键约束

Moonlight 生成随机 4 位 PIN → 用 `salt` + PIN 派生 AES 密钥 → 在 `/pair?phrase=getservercert`
里把 `clientcert`、`salt` 发给 Sunshine → Sunshine 等管理员在 Web UI/托盘输入这个 PIN →
双方用同一 PIN 完成 5 阶段握手 → Sunshine 把客户端证书写入 approved clients。

**PIN 是双方共享的对称秘密**，Sunshine 必须知道 PIN 才能解密响应。所以“免 PIN”不能只删掉
UI，必须让两端各自算出同一个 PIN。

## 已实现的方案：客户端证书派生的确定性 PIN + 指纹白名单

### 1. PIN 派生（两端必须逐字节一致）

```
digest = SHA-256( "LANPROJECTION-PAIR-v1" || clientCertDER )
value  = digest[0..3] 按大端解释为 uint32
PIN    = value % 10000，格式化为 4 位十进制（前导补 0）
```

只使用**客户端**证书，因为客户端在配对开始前还没有拿到服务端证书，必须先算出 PIN。

- 输入只有公开证书的 DER 编码，不含任何私钥；
- 换大屏 = 换客户端证书 = PIN 自动不同；
- PIN 本身不再承担“防 MITM”职责，授权由第 2 步的白名单承担。

### 2. 授权（Sunshine 侧，决定谁可以免 PIN）

Sunshine 配置目录新增 `enterprise-authorized-clients.json`（与 `sunshine_state.json` 同目录）：

```json
{
  "version": 1,
  "devices": [
    { "name": "会议室大屏-3F", "fingerprint": "<client-cert-DER 的 sha256 小写 hex>" }
  ]
}
```

请求进入 `start_pairing_session`（`src/nvhttp.cpp`，`case ADDED` 之后）时：

1. 取本次会话的 `clientcert` PEM → DER → SHA-256 指纹；
2. 指纹在白名单内 → 用上面的算法派生 PIN，**直接执行 `getservercert` 阶段**，不进入“等待管理员
   输入”状态，并写审计日志（客户端地址、设备名、指纹前 12 位）；
3. 指纹不在名单内，或文件不存在/为空 → 完全走原版流程（提示 PIN）。

白名单文件不存在时功能整体禁用，行为与原版 Sunshine 一致。

### 3. Moonlight 侧（原本生成 PIN 的地方）

- `app/backend/enterprisepairing.cpp` 用同一算法从本机证书派生 PIN；
- `ComputerManager::generatePinString()` 在 `enterprisePinlessPairing` 开启时返回派生 PIN，
  否则返回随机 PIN；
- `app/gui/PcView.qml` 在开启时**不弹出** `pairDialog`（没有需要输入的东西），配对完成后
  直接进入应用列表。

### 4. 开关与回退

- Moonlight：设置项 `enterprisePinlessPairing`（QSettings 键 `enterprisepinless`），
  **缺省开启**，因为本产品两端同时分发；连原版 Sunshine 时需手动关闭。
- Sunshine：由白名单文件是否存在隐式控制，存在即启用。删除文件即回到原版行为。
- 任一端关闭 → 退回原版随机 PIN 流程，两端都兼容原有协议。

## 安全说明（重要）

派生 PIN 完全由公开材料算出，因此它**不是秘密**。真正的授权边界是：

- Sunshine 的指纹白名单（谁可以免 PIN）；
- 客户端必须持有该证书对应的私钥才能完成 TLS 握手。

也就是说免 PIN 配对的安全强度等于“把这张客户端证书当作设备身份”，与直接用 mTLS 白名单
等价。派生 PIN 只是让协议能在无人工干预下走完。

部署建议：

- 白名单文件随花名册/镜像下发，不要让普通员工可写；
- 大屏证书一旦泄露或换机，立即从白名单删除并重新配对；
- 审计日志里只出现指纹前缀，不出现私钥内容。

## 改动点

| 仓库 | 文件 | 改动 |
| --- | --- | --- |
| sunshine | `src/enterprise_pairing.{h,cpp}` | 新增：指纹计算、PIN 派生、白名单加载与查询 |
| sunshine | `src/nvhttp.cpp` | `start_pairing_session()` 加白名单自动批准分支 + 审计日志 |
| sunshine | `cmake/compile_definitions/common.cmake` | 把新源文件加入 `SUNSHINE_TARGET_FILES` |
| moonlight | `app/backend/enterprisepairing.{h,cpp}` | 新增：客户端侧同一套 PIN 派生 |
| moonlight | `app/backend/computermanager.cpp` | `generatePinString()` 企业模式下返回派生 PIN |
| moonlight | `app/settings/streamingpreferences.{h,cpp}` | 新增 `enterprisePinlessPairing` 设置项 |
| moonlight | `app/gui/PcView.qml` | 企业模式下不弹 PIN 对话框 |
| moonlight | `app/app.pro` | 注册新源文件 |
| 本仓库 | `scripts/enterprise-whitelist.ps1` | 从大屏证书生成/追加白名单文件 |

## 使用方法

1. 在大屏上首次运行 Moonlight 分支，让它生成证书（会自动创建）。
2. 在大屏上生成白名单（读取本机 Moonlight 身份）：

   ```cmd
   powershell -ExecutionPolicy Bypass -File scripts\enterprise-whitelist.ps1 -Name "会议室大屏-3F"
   ```

   或从导出的 PEM 证书生成：`-CertificatePath .\receiver.crt`。

3. 把生成的 `enterprise-authorized-clients.json` 放到每台员工电脑的 Sunshine 配置目录
   （与 `sunshine_state.json` 同目录）。
4. 员工电脑启动 Sunshine 分支，大屏在 Moonlight 里添加该主机并点配对——不再出现 PIN。

## 验证用例

1. 白名单证书 + 两端开启 → 5 阶段握手成功，Sunshine 日志有 `enterprise pairing:
   auto-approving authorized receiver`，无 PIN 交互；
2. 非白名单证书 → 仍出现 PIN 提示，未输入则超时失败；
3. 白名单内但客户端被换成别的证书 → 派生 PIN 不同，握手失败；
4. 删除白名单文件 → 回到原版随机 PIN 流程可正常配对。

## 实测结果（2026-11-16，本机回环）

构建产物：`sunshine.exe` 458,160,256 字节、`Moonlight.exe` 2,822,144 字节。

1. 大屏侧发起 `Moonlight.exe pair NINE-PC01`（Sunshine 的 mDNS 实例名），四个 HTTP 阶段
   `phrase=getservercert` → `clientchallenge` → `serverchallengeresp` → `clientpairingsecret`
   全部一次通过，随后 `https://<host>:47984/pair?...phrase=pairchallenge` 返回成功。
   全过程约 1 秒，**没有出现 PIN 对话框**。
2. Sunshine 侧同一时刻只有两条相关日志：

   ```text
   Info: enterprise pairing: 1 authorized client(s) loaded from ...\config\enterprise-authorized-clients.json
   Info: enterprise pairing: auto-approving authorized receiver roth from 28.0.0.1 (cert e427a7619aaf...)
   ```

3. `clientpairingsecret` 阶段用派生 PIN 做 AES 校验并能通过，说明**两端派生算法逐字节一致**；
   Sunshine 把客户端证书写入 `sunshine_state.json` 的 `root.named_devices[0]`（`"enabled": "true"`）。
4. 非白名单证书（`CN=unauthorized-smoke`，指纹 `32c273a0…ef6b`）只打印指纹、无 `auto-approving`，
   回落到等待人工 PIN 的原版行为——即白名单确实生效。
5. 配对完成后进入 `Desktop` 应用**成功出画面**，Sunshine 日志出现
   `Creating encoder [hevc_nvenc]`、`NvEnc: created encoder v1301 HEVC P1 async two-pass rfi sfe-auto`、
   `/CN=NVIDIA GameStream Client -- verified`、`DESTINATION :: /applist`。

### 实现过程中修掉的一个缺陷

初版在 `start_pairing_session()` 里调用 `getservercert()` 之后直接 `return false`，指望
`util::fail_guard` 负责写响应——实测该路径**不会发送响应**，客户端只能等超时。
现改为显式写响应（与人工 PIN 路径一致）：

```cpp
getservercert(sess, tree, pin);
if (sess.failed) {
  map_id_sess.erase(sess_it);
  return false;
}
return write_pairing_response(sess, tree);   // nvhttp.cpp:559
```

### 部署注意

- 配对成功后 Moonlight 会弹出「配对成功」对话框，**必须点确定**，否则 `srvcert` 不会写回
  `HKCU\Software\Moonlight Game Streaming Project\Moonlight\hosts\<n>`；此时强杀进程会留下空 `srvcert`。
- 白名单读取的是 `file_state` 同目录下的 `enterprise-authorized-clients.json`；
  Sunshine 没有 portable 模式，用绝对路径配置 `file_state` 最稳。
- Moonlight 的 `certificate` 注册表值是 QSettings 的 `@ByteArray(...)` 包装，
  生成白名单时必须先截取 PEM 块（`scripts/enterprise-whitelist.ps1` 已处理）。

## 后续（不在本版本）

- 由控制服务器下发/轮换白名单，免去手工拷贝文件；
- 证书吊销与审计查询页面；
- 首次注册时的管理员确认流程。