# 系统镜像与烧录

> **结论：用官方镜像就行，不需要自建镜像，也不需要分享镜像。**

实测官方 **`tspi-1m-kernel6.1-debian-3.1inch-mipi-20260331.img`** 已经带 3.1" MIPI
面板支持，刷完直接跑本项目的 `setup_device.sh` 即可（我们在这套镜像上验证通过）。

---

## 1. 下载官方镜像

立创泰山派资料下载中心：<https://wiki.lckfb.com/zh-hans/tspi-rk3566/download-center.html>

- 百度网盘：<https://pan.baidu.com/s/5m1jN3xFWAu_8qbUbidNeMw>
- 路径：`立创·泰山派开发板资料 / 第06章.【立创·泰山派】系统镜像 / kernel6.1 / 【镜像】Debian12 /`
- 文件：**`tspi-1m-kernel6.1-debian-3.1inch-mipi-20260331.img`**（4.46 GB）

同目录下还有 HDMI 版和 `6.1boot.img` 等，**不要选错**：

| 文件 | 说明 |
|---|---|
| `tspi-1m-kernel6.1-debian-3.1inch-mipi-20260331.img` | ✅ 3.1" MIPI 版，本项目用的就是它 |
| `tspi-1m-kernel6.1-debian-hdmi-20260320.img` | HDMI 版，MIPI 屏点不亮 |
| `6.1boot.img` | 只包含 boot 分区，用于单独更新内核 |

---

## 2. 烧录

工具：**RKDevTool** + **DriverAssitant**（同在资料包 `第06章` 目录）。

1. 板子断电，USB 线接 Type-C（OTG）口到电脑；
2. 按住 **Recovery/SW** 键，再上电，保持 3 秒后松开；
3. RKDevTool 显示「发现一个 LOADER 设备」；
4. 切到 **「升级固件」** → 固件选 `.img` → **「升级」**；
5. 等待「下载完成」（约 3–6 分钟），板子自动重启。

默认账号 **`linaro` / `linaro`**（root 同密码）。

---

## 3. 两个必踩的坑（官方镜像本身的问题）

### sshd 起不来：没有 host key

现象：`ssh: connect to host ... port 22: Connection refused`，串口里看：

```
sshd: no hostkeys available -- exiting
```

解决：

```bash
ssh-keygen -A
systemctl restart ssh
```

（`setup_device.sh` 会先检查并自动处理。）

### 机芯网卡没被 NetworkManager 接管

官方镜像首次启动时 `usb0`（机芯的 NCM 网卡）是 `unavailable` 状态，
要么不配 IP，要么 DHCP 等不到，导致上位程序连不上机芯。

解决（`setup_device.sh` 已包含）：

```bash
nmcli device set usb0 managed yes
nmcli con mod 4117 connection.autoconnect yes
nmcli con up 4117
```

检查：

```bash
ip -4 addr show usb0        # 应为 192.168.64.32/24
ping -c2 192.168.64.64
```

---

## 4. 部署本项目

```bash
git clone <本项目仓库> ~/linux-app
cd ~/linux-app
bash scripts/setup_device.sh
```

脚本依次完成：

1. 检查架构（aarch64 / Debian）
2. 生成 SSH host key（如果缺）
3. `apt-get install --no-upgrade` 装依赖（**不升级已装包**，避免把 network-manager 顶掉导致断网）
4. `scripts/fetch_lvgl.sh` 拉 LVGL 9.2.2（GitHub → Gitee 兜底）
5. 编译
6. 写 udev 规则把机芯网卡固定成 `usb0`，建 `192.168.64.32/24` 的 NM 连接并**激活**
7. 关闭 lightdm、切 `multi-user.target`、装 systemd 服务并启动

跑完屏幕应显示热像画面，日志里能看到：

```
relay: :8081 -> 192.168.64.64:80
rtsp: rtsp 320x240 codec=mjpeg
[..] frames=258 max=27.0 min=22.1 ctr=24.7
```

---

## 5. 机芯设置

机芯需要先切到 **NCM 模式**，IP 改为 `192.168.64.64`（出厂多为 UVC 模式或 `10.0.0.x`）。
改法见 [ncm_switch.md](ncm_switch.md)，接口清单见 [4117_isapi_verified.md](4117_isapi_verified.md)。

## 6. 屏幕不亮时

```bash
systemctl get-default          # 应为 multi-user.target
pgrep -a lightdm || echo ok    # 不应有输出
journalctl -u tb4117.gui.service -b -n 40
```

如果 Xorg 还占着 DRM，`/dev/dri/card0` 会被 `lightdm` 持有，LVGL 无法提交画面。
