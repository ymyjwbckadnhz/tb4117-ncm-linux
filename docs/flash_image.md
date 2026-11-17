# 系统镜像与烧录

官方 Debian 镜像不带 3.1" MIPI 屏支持（面板设备树缺失，屏幕点不亮），
所以本项目需要一个**自编译的 boot 分区**。有两种部署路径：

| 路径 | 需要下载 | 适合 |
|---|---|---|
| **A. 只换 boot 分区**（推荐） | `6.1boot.img`（**40 MB**） | 已经刷了官方 Debian 6.1 镜像 |
| **B. 整机镜像** | `6.1update.img`（4.46 GB） | 想要开箱即用 |

两条路径最终都要跑一次 `scripts/setup_device.sh`（装依赖、编译、装服务）。

---

## 路径 A：只刷 boot 分区（40 MB）

### 文件

| 文件 | 大小 | SHA256 |
|---|---|---|
| `6.1boot.img` | 41 563 648 B | `fc461a426906efa87ea61dbe97b0a0fed448ea76fb4f6eccfdc8e1e3166f9b64` |

校验：

```bash
sha256sum 6.1boot.img
# Windows: Get-FileHash -Algorithm SHA256 6.1boot.img
```

里面是**内核 6.1.141 + 设备树**，已确认包含 3.1" 面板节点
（`simple-panel-dsi`、`dsi1_panel`、`panel-init-sequence`）。

### 步骤

1. 先按官方教程把 **Debian 6.1 镜像**刷进板子（RKDevTool「升级固件」）；
2. 重新进入 Loader 模式，RKDevTool 切到 **「下载镜像」** 页；
3. 在分区表里找到 **`boot`** 分区，勾选它，右侧「...」选 `6.1boot.img`；
4. 点 **「执行」**，只写 boot 分区（几秒完成）；
5. 重启，屏幕应点亮。

> 内核版本必须与 boot.img 一致（6.1.141）。板子上的模块直接放在 `/lib/modules/`
> （不带版本目录），所以换 boot 分区不会因为模块路径不匹配而失效。

### 之后

```bash
git clone <本项目仓库> ~/linux-app && cd ~/linux-app
bash scripts/setup_device.sh
```

---

## 路径 B：整机镜像（4.46 GB）

| 文件 | 大小 | SHA256 |
|---|---|---|
| `6.1update.img` | 4 787 542 602 B | `05cc5eb63dd44c76d4200449d7b323fa8540d0b1cba87eb2d92590a4ddd0f021` |

1. RKDevTool 切到 **「升级固件」** 页；
2. 「固件」选 `6.1update.img`；
3. 「升级」，等待「下载完成」（约 3~6 分钟）；
4. 板子自动重启，默认账号 **`linaro` / `linaro`**（root 同密码）。

然后同样跑一次 `scripts/setup_device.sh`。

> 4.46 GB 超过 GitHub Release 单文件 2 GB 上限，需要分卷（`7z a -v1500m`）
> 或用网盘分享；40 MB 的 `6.1boot.img` 可以直接作为 Release 附件，更推荐。

---

## 进入 Loader / Maskrom 模式

1. 板子**断电**；
2. USB 线接板子的 **Type-C（OTG）** 口与电脑；
3. 按住 **Recovery/SW** 键不放，再上电，保持 3 秒后松开；
4. RKDevTool 底部显示「发现一个 LOADER 设备」。

工具与驱动（`RKDevTool`、`DriverAssitant`）在立创泰山派资料包的
`第06章.【立创·泰山派】系统镜像` 目录里。

---

## 机芯设置

机芯要先切到 **NCM 模式**，并把 IP 改成 `192.168.64.64`（出厂多为 UVC 模式或 `10.0.0.x`）。
改法见 [ncm_switch.md](ncm_switch.md)，接口见 [4117_isapi_verified.md](4117_isapi_verified.md)。

首次上电后如果屏幕不亮：

```bash
# 1) 确认机芯链路起来了
ip -4 addr show usb0        # 应为 192.168.64.32/24
ping -c2 192.168.64.64

# 2) 确认没有 X 占着 DRM
systemctl get-default       # 应为 multi-user.target
pgrep -a lightdm || echo "lightdm 已关闭"

# 3) 看服务日志
journalctl -u tb4117.gui.service -b -n 40
```

---

## 关于镜像的授权

镜像是基于立创泰山派开源的 Linux SDK 自编译的（内核 6.1 + 3.1" 面板 DTS），
内核遵循 **GPL-2.0**；修改点（面板设备树、触摸、GP7101 背光）都在本仓库与
资料包中可查。分发镜像时请保留相同的开源义务。
