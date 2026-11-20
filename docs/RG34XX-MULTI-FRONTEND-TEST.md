# RG34XX 多前端移植测试说明

**目标设备：Anbernic RG34XX / Allwinner H700**  
**测试分支：`test/rg34xx-multi-frontend`**  
**状态：控制中心修复版完整镜像已通过 CI 构建；A/B 升级包构建与真实镜像回环校验已加入 CI；尚未完成 RG34XX 实机验收。**

## 移植原则

本分支沿用 RG34XX 原有 H700 启动链、U-Boot、内核、设备树、Wi-Fi/蓝牙及硬件驱动。多前端管理只修改 BaseOS 的 userspace overlay、服务和控制界面，不要求 Slot、NextUI、spruceOS 或其他前端增加 BaseOS 专用代码，也不向前端程序目录写入 manifest 或启动脚本。

选择器和诊断页使用独立的 `baseos-control-center` framebuffer 程序。H700 当前内核没有 framebuffer console，不能直接复用 RG353PS 的终端式 UI。

## 开机入口和按键

- 开机时按住 **L1 或 R1 任意一个**，进入 BaseOS 前端选择器；不要求同时按下两个肩键。
- **MENU** 仍是 USB Mass Storage 维护入口，优先级高于前端选择器。
- 未按 L1/R1 时，按配置的默认前端启动；没有可用前端时进入 BaseOS 诊断/选择界面。
- L1/R1 请求在 `rcS` 中读取一次，再由 `frontend-session` 消费一次，避免 init respawn 时重复弹出。

选择器交互：
- 方向键：移动，首尾循环（在第一项按上跳到末尾，在最后一项按下回到第一项）；禁用的前端 profile 自动排列在列表末尾，不会挤占启用项的优先位置。
- A：临时启动当前前端，不改默认项；在列表末尾的操作行中，A 用于打开新建 profile 或诊断页面。
- Start：保存当前 profile 为默认前端并启动。当前默认项在列表右侧显示 **默认** 徽标，标题栏不再重复显示默认名称。
- SELECT：启用/停用当前 profile。
- L1：删除当前托管/兼容 profile 的配置文件；同包其他 profile 仍在时保留共享程序，删除最后一个 profile 时才清理对应的程序包/兼容包装；不会删除 profile userdata、Games 或 Saves。
- X：确认删除整个前端程序包。托管前端、自动兼容前端和旧式 adapter 会分别调用对应的卸载路径。
- Y：重命名当前行的 profile 显示名称（只改 `display_name`，不改 profile ID 和 userdata）。
- R1：重命名整个前端文件夹（`Frontends/<名称>`），并整体迁移包身份。兼容前端会一起迁移 `.baseos/compat/<包名>` 元数据目录、`launch.sh` 里的程序路径、`.baseos/instances/` 实例数据、payload/停用标记、path map 和保存的 `frontend_id`，不会在 catalog 里留下旧 id 的第二条；托管前端会迁移实例数据并把旧包名写入 `previous_package_id`。身份不一致的兼容包（sidecar 的 `compat_source` 与包名不符）会拒绝改名和删除，避免删掉另一个前端的程序目录。
- （机身上的印刷 X 在 H700 设备树中报 `BTN_NORTH`，印刷 Y 报 `BTN_C`，R1 报 `BTN_Z`；界面提示一律按印刷字符给出。）
- B：退出选择器，继续使用已有默认项。
- 列表末尾只保留 **新建配置…** 一个入口。光标停在操作行时，用左右键在可用前端之间切换新建配置的“来源”，该行第二行显示 `为 <前端名> 新建配置`；选定来源后按 A 进入虚拟键盘。移动光标本身不会改变来源（以前上下移动就会跟着变，容易选错）。新建时复制该 profile 的配置项并生成独立 profile ID。操作使用机身内置虚拟键盘，不需要把 TF 卡接到电脑；键盘的 CAP 键切换大小写后，字母键帽会同步显示大写并且 CAP 键格高亮。
- 每条 profile 第二行说明它的运行方式：**BaseOS 托管**（BaseOS 自己安装和管理的包）、**兼容运行**（第三方前端，由 BaseOS 的兼容层用自己的运行时拉起，前端文件不改动）、**旧版适配**（历史 adapter）、**原生程序**（卡根入口契约）。被停用的条目在右侧显示 **已禁用** 徽标。
- 诊断页包含系统/存储/电池状态和显眼的 **BUTTON TEST** 入口。按键测试页显示掌机按键布局（ABXY 按任天堂排列：X 上、Y 左、A 右、B 下），按键/摇杆输入会点亮对应区域；L3/R3 和左右摇杆始终显示。测试页使用 **SELECT + START** 退出，先按哪个都行，第二下在 1.2 秒内按下即可；A/B 不承担返回功能，因此可以正常测试。

## 自动发现与零修改兼容

BaseOS 先扫描当前启动使用的用户数据卡中的 `Frontends/`，并读取旧式 adapter。对识别到的普通前端目录，在卡内 `.baseos/compat/` 生成兼容 sidecar；不会在原前端目录内生成文件、改名或替换 ELF。

兼容匹配以目录结构和启动契约为依据，不读取或判断前端产品名。对于没有启动脚本的 H700 卡根二进制，必须遵守明确的 `System/slot` 或 `System/frontend` 命名契约，避免把动画、libretro 核心或辅助 ELF 误认为前端。当前规则为：
- 前端包根目录中的 `launch.sh`、`start.sh`、`run.sh` 或 `frontend.sh`；
- 前端包根目录中的 `*.zip`，且归档内含有 `.system/<platform>/paks/<package>.pak/launch.sh`（卡根载荷归档）；
- 任意 `.system/h700/paks/<package>.pak/launch.sh`；
- 任意一级子目录中的 `scripts/runtime.sh`；
- `System/launch_frontend.sh`；
- H700 卡根 ELF 契约：仅识别 `System/slot` 或 `System/frontend`，并检查 ELF magic；不会扫描 `System/` 中的任意 ELF，也不会把包根目录的任意 `*.elf` 当作前端入口。

现有前端只是这些通用规则的兼容样例：
- Slot：`System/slot`
- NextUI：卡根载荷归档 `MinUI.zip`（由 BaseOS 展开，见下）
- spruceOS：`spruce/scripts/runtime.sh`
- 通用 H700 前端：`System/frontend` 或 `System/launch_frontend.sh`
- 历史 adapter：`.baseos/frontends/<id>/launch.sh` 或 `baseos/frontends/<id>/launch.sh`

扫描只接受明确的入口名称，不会尝试执行目录里的任意 ELF。自动生成的兼容配置、停用标记和路径映射均归 BaseOS 管理；原始前端目录作为输入保留。

### 卡根载荷归档

NextUI 这类 MinUI 分支不把程序放在 `Frontends/<id>/` 内，而是写死 `SDCARD_PATH=/mnt/SDCARD` 和 `SYSTEM_PATH=$SDCARD_PATH/.system/<platform>`，只能从卡根运行。它的官方发布包（`MinUI.zip` 或 `*-all.zip`）直接放进 `Frontends/<id>/` 即可，不需要手动解压，也不需要丢弃归档格式：

- 首次启动时 BaseOS 把归档展开到卡根，并在 `.baseos/payload/<id>.conf` 记录归档的 `mtime:size`、入口和展开出来的私有根目录（`.system`、`.tmp_update`、`Tools`）；发布包里其他 H700 机型的分支（`tg5040`、`tg5050`、`desktop` 等）会被跳过，只解当前 `BASEOS_PLATFORM` 对应的那一支，安装在 vfat 卡上从 305 MB 降到 99 MB；
- 归档未变化时后续启动直接复用，不重复展开；用户替换成新版归档后自动重新展开；
- 删除前端（`delete-compat`）会移除这些私有目录；同一层的 `Roms`、`Bios`、`Saves` 等共享数据树不在记录范围内，不会被清理；
- 两个前端声明同一个私有根时，删除其中一个会保留该目录，直到最后一个引用者被删除。

这类前端的型号检测来自 `/mnt/vendor/bin/dmenu.bin`，而 BaseOS 用占位文件替代了它。因此展开启动时 BaseOS 会把 `/etc/baseos-release` 中的 `BASEOS_MODEL_STRING` 以 `RGXX_MODEL` 传给前端，避免它回落到 `rg35xxplus` 的按键布局和屏幕参数。

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

BaseOS 使用一组固定的**规范公共目录**作为共享数据的实际存放位置，并允许每个前端继续使用自己原本认识的目录名称。路径映射配置中的 `*_target` 项就是前端侧的目录名称；对应的 `share_*` 项决定该项目是否接入规范公共目录。

例如，规范遮罩目录是 TF 卡根目录的 `Overlays/`。如果前端 1 的源码固定读取 `Overlay/`（单数），前端 2 读取 `Overlays/`（复数），两者都要共享同一套遮罩，则分别配置：

```ini
# 前端 1：保留它原本读取的目录名，映射到公共 Overlays
overlays_target=Overlay
share_overlays=yes

# 前端 2：它使用的名称刚好就是规范目录名
overlays_target=Overlays
share_overlays=yes
```

BaseOS 会在前端会话期间把前端 1 所需的根目录入口 `Overlay/` 映射到规范目录 `Overlays/`；前端 2 则直接使用规范目录 `Overlays/`。因此，两个前端实际读写的是同一份文件。遮罩文件应放在 TF 卡根目录的 `Overlays/`，而不是某个前端程序目录中。其他支持的规范公共目录也遵循同一思路，例如 `Roms/` 可以映射到 `Games/`。

每个前端有一份独立的 BaseOS 路径映射配置，保存在活动数据卡的 `.baseos/frontend-paths/<frontend-id>.conf`。首次扫描时自动生成；后续扫描不会覆盖用户修改。配置按前端 ID 区分，同一前端的不同 profile 共用这份目录映射规则，但各自的 profile userdata 仍然隔离。编辑时应修改对应前端 ID 的配置文件，而不是修改前端程序目录。

默认情况下，前端包中的普通顶层目录（包括隐藏目录）都按私有目录处理，运行时映射回该前端自己的目录。对于不属于标准路径项目、但希望映射到公共目录的前端目录，可在该前端自己的配置文件中增加一个或多个 `shared_map`：

```ini
# 前端的 media 目录映射到 BaseOS 公共 Overlays
shared_map=media:Overlays

# 前端的 cheatfiles 目录映射到 BaseOS 公共 Cheats
shared_map=cheatfiles:Cheats
```

格式为 `shared_map=<前端包内目录名>:<BaseOS公共目录名>`。目前公共目标限定为 `Games`、`Saves`、`Bios`、`Cheats`、`Overlays` 和 `Shaders`。映射源目录必须存在于该前端包内；未声明的目录仍保持私有。BaseOS 只创建会话期间的目录别名/挂载，不搬移或删除源文件；已有的非空卡根同名目录会受到保护，冲突时不会强行覆盖。若目标名称与规范目录名称仅大小写不同（例如 FAT/exFAT 上的 `Overlay` 与 `Overlays` 不同，不是仅大小写差异），按正常名称映射处理；大小写仅有差异时不会重复创建别名。

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

## 本轮控制中心与 USB 存储补充

- 前端选择器和系统诊断默认使用简体中文；列表末尾可切换为 English，诊断页按 L1 切换。选择结果写入 /data/baseos-ui-language，A/B 系统更新不会重置该设置。
- 诊断页的 USB 大容量存储入口会请求下一次启动进入维护模式，然后重启；开始前先连接数据线。它复用 MENU 启动入口的安全检查：数据分区必须先卸载，整个 TF2 磁盘（若存在）或 TF1 的 p7 分区才会交给 USB 主机。
- 存储 LUN 仍为可移动、可写模式，并保留 SCSI FUA。传输完成且主机不再读写后可以直接拔掉数据线，不再要求先执行系统“弹出”。不要在复制/写入过程中拔线；主机仍可能有尚未提交的缓存写入，FUA 不能替代主机操作系统对未完成写入的刷新。
- A/B 升级器在拒绝包或写入、回读校验、槽位切换失败时，会把包改名为 .bosupd.failed，避免下次开机自动重复失败；原包保留在原目录以供检查/手动恢复。具体原因写入 /data/update/last-failure 并记录到 baseos-boot.log。成功提交的 .bosupd 仍按原流程删除。


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
