# moonlight-qt-Castscreen · Windows 会议室接收端

Moonlight Qt 定制分支。大屏保持待机，员工 Sunshine-Castscreen 主动发现并请求投屏；自动握手及精确 UUID/证书持久化后启动串流。包含原生会议室名称、状态、防火墙设置和取消/停止。保留上游许可证和 README。

初始化：git submodule update --init --recursive。

构建入口 scripts/build-moonlight-native.ps1。需要 Qt 6.12.0 MSVC2022 SDK、VS C++工具和 Moonlight v20.1 Windows依赖，使用 -Prepare 安装仓库本地依赖，或 -QtRoot 指定现有SDK。默认构建输出为仓库相邻 build-moonlight-qt-release，不污染源码。

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build-moonlight-native.ps1 -Prepare -Python python
powershell -ExecutionPolicy Bypass -File scripts/build-moonlight-native.ps1 -CheckOnly
```

installer/receiver.iss 版本1.0.1，输入 artifacts/package/receiver。先准备 Moonlight.exe、Qt插件和运行依赖，再使用 Inno Setup 6 编译；不可只打包EXE。日志/receiver.conf/配对数据库不可进入发布源或Git。

回归入口 tests/receiver-regression/run.ps1，需要已构建App对象、Qt路径与本机MSVC环境，MSVC通过vswhere自动查找，也可用 -VcVars 指定环境脚本。此次目录迁移未重跑完整Qt构建或真实视频；既有验证范围见docs/castscreen。
