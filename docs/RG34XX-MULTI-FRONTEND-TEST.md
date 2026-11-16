# RG34XX 多前端移植测试说明

**目标设备：Anbernic RG34XX / Allwinner H700**  
**测试分支：`test/rg34xx-multi-frontend`**  
**状态：控制中心修复版完整镜像已通过 CI 构建；A/B 升级包正在加入 CI；尚未完成 RG34XX 实机验收。**

## 移植原则

本分支沿用 RG34XX 原有 H700 启动链、U-Boot、内核、设备树、Wi-Fi/蓝牙及硬件驱动。多前端管理只修改 BaseOS 的 userspace overlay、服务和控制界面，不要求 Slot、NextUI、spruceOS 或其他前端增加 BaseOS 专用代码，也不向前端程序目录写入 manifest 或启动脚本。

选择器和诊断页使用独立的 `baseos-control-center` framebuffer 程序。H700 当前内核没有 framebuffer console，不能直接复用 RG353PS 的终端式 UI。

## 开机入口和按键

- 开机时按住 **L1 或 R1 任意一个**，进入 BaseOS 前端选择器；不要求同时按下两个肩键。
- **MENU** 仍是 USB Mass Storage 维护入口，优先级高于前端选择器。
- 未按 L1/R1 时，按配置的默认前端启动；没有可用前端时进入 BaseOS 诊断/选择界面。
- L1/R1 请求在 `rcS` 中读取一次，再由 `frontend-session` 消费一次，避免 init respawn 时重复弹出。

选择器交互沿用 RG353PS：
- 方向键：移动。
- A：临时启动当前前端，不改默认项。
- Start：保存为默认前端并启动。
- SELECT：启用/停用。
- Y：确认删除当前托管包 profile。
- X：确认删除整个前端程序包。
- B：退出选择器，继续使用已有默认项。
- 诊断页包含系统/存储/电池状态和按键测试。

## 自动发现与零修改兼容

BaseOS 先扫描当前启动使用的用户数据卡中的 `Frontends/`，并读取旧式 adapter。对识别到的普通前端目录，在卡内 `.baseos/compat/` 生成兼容 sidecar；不会在原前端目录内生成文件、改名或替换 ELF。

兼容匹配以目录结构和启动契约为依据，不读取或判断前端产品名，也不要求入口文件使用固定的产品 basename。当前通用规则为：
- 前端包根目录中的 `launch.sh`、`start.sh`、`run.sh` 或 `frontend.sh`；
- 任意 `.system/h700/paks/<package>.pak/launch.sh`；
- 任意一级子目录中的 `scripts/runtime.sh`；
- `System/` 目录中通过 ELF magic 检查的任意常规文件；
- 前端包根目录中通过 ELF magic 检查的 `*.elf`。

现有前端只是这些通用规则的兼容样例：
- Slot：`System/slot`
- NextUI：`.system/h700/paks/MinUI.pak/launch.sh`
- spruceOS：`spruce/scripts/runtime.sh`
- 通用 H700 前端：`System/frontend` 或 `System/launch_frontend.sh`
- 历史 adapter：`.baseos/frontends/<id>/launch.sh` 或 `baseos/frontends/<id>/launch.sh`

扫描只接受明确的入口名称，不会尝试执行目录里的任意 ELF。自动生成的兼容配置、停用标记和路径映射均归 BaseOS 管理；原始前端目录作为输入保留。

## 托管前端包与 profile

推荐的托管布局与 RG353PS Multi-Frontend API v1 一致：

```text
/mnt/sdcard/
├── Games/                         # 所有前端共享的 ROM 根目录
└── Frontends/
    └── <package-id>/
        ├── baseos-frontend.conf
        ├── launch.sh
        ├── payload/                # 可选：共享程序文件
        └── profiles/
            ├── default.conf
            └── <another-profile>.conf
```

一个 package 的多个 profile 共用 payload，但每个 profile 有独立的 BaseOS instance、userdata、配置、收藏/最近游玩和状态目录。无需复制程序文件即可创建第二套独立配置。

## 共享与隔离策略

| 数据 | 策略 |
|---|---|
| ROM 游戏本体 | 所有前端/profile 共享，规范目录为 `Games/` |
| 电池存档（如 `.sav`） | 共享，规范目录为 `Saves/` |
| 模拟器核心 | 使用 H700 共享核心目录；不覆盖前端自带核心 |
| 收藏夹、最近游玩 | 按 profile 隔离 |
| 前端偏好、核心选择、即时存档/恢复状态 | 按 profile 隔离 |
| BIOS、cheats、overlays、shaders 等素材 | 按路径映射策略配置；默认不把未知目录强行共享 |
| 前端程序 payload | 同一 package 的 profile 之间共享 |

BaseOS 会为需要旧目录名称的前端创建运行时路径映射，例如把前端要求的 `Roms/` 指向共享的 `Games/`，把 profile 私有配置/素材指向对应 instance。路径别名由 BaseOS 在前端会话结束时清理。

## 构建与下载

专用 GitHub Actions 工作流只监听本测试分支，使用仓库内已校验的 RG34XX prepared inputs，执行：
1. 多前端发现、profile 生命周期、兼容包装器、路径映射和会话测试；
2. H700 framebuffer 控制中心静态编译；
3. RG34XX rootfs 构建和 QEMU userspace 启动检查；
4. 完整 RG34XX SD 镜像组装及结构校验。

成功后上传两个独立的临时 Actions artifact，不创建正式 Release 或版本标签：

- **完整刷写包**：`baseos-rg34xx-*-multi-frontend-test.img.zip`，用于首次安装或恢复分区布局；会重建整张卡，刷写前必须备份数据。
- **A/B 升级包**：`baseos-rg34xx-<VERSION>.bosupd`，附带归档 SHA-256 和构建信息。CI 会在上传前把升级包实际应用到一份复制的 RG34XX 镜像，检查槽位切换，并确认 UDISK 和用户 FAT 分区保持逐字节不变。

日常测试优先使用 A/B 升级包：将 `.bosupd` 复制到当前活动的数据卡根目录（TF2 为活动卡时放 TF2，否则放 TF1），安全弹出后正常开机。BaseOS 会在启动前应用更新、校验并自动重启；更新过程中不要断电或拔卡。成功提交后升级文件会自动删除。

升级包只适用于已经刷入 RG34XX BaseOS 七分区 A/B 布局的设备，不能从原厂分区布局直接升级，也不能跨设备型号使用。它更新非活动 rootfs 槽，不重刷整张卡；`/data`、`Games/`、`Saves/`、前端程序和用户配置均保留。若设备尚未刷入本项目的 A/B 完整镜像，首次仍需完整刷写。

## 首次实机验收

请用备用 TF 卡进行第一次刷写。完整镜像会重建目标卡分区，刷写前备份卡内所有数据。首次验收重点：
- L1 单独按住开机能打开选择器。
- R1 单独按住开机能打开选择器。
- MENU 仍进入 USB Mass Storage，不能误进选择器。
- Slot、NextUI、spruceOS 都能被识别并正常启动，原始前端目录不被改写。
- 两个 profile 共用 Games/ 和电池存档，但收藏、设置与即时存档互相隔离。
- SELECT、Y、X 的启停/删除边界正确，卸载不删除共享游戏和 profile instance data。
- 诊断页、按键测试、无前端时的诊断入口正常。
- 前端退出、USB 存储模式和系统重启后，不遗留临时路径别名或挂载。

该测试构建的 CI/QEMU 结果不能代替上述真实硬件验收。
