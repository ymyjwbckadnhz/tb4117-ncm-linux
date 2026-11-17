# 系统镜像与烧录

> **只用官方镜像就行。** 立创的 `tspi-1m-kernel6.1-debian-3.1inch-mipi-20260331.img`
> 自带 3.1" MIPI 面板支持，本项目就在这套镜像上实测通过——**不需要自建或分发任何镜像**。

---

## 1. 下载官方镜像

立创泰山派资料下载中心：<https://wiki.lckfb.com/zh-hans/tspi-rk3566/download-center.html>

- 百度网盘：<https://pan.baidu.com/s/5m1jN3xFWAu_8qbUbidNeMw>
- 路径：`立创·泰山派开发板资料 / 第06章.【立创·泰山派】系统镜像 / kernel6.1 / 【镜像】Debian12 /`
- 文件：**`tspi-1m-kernel6.1-debian-3.1inch-mipi-20260331.img`**（4.46 GB）

| 文件 | 说明 |
|---|---|
| `tspi-1m-kernel6.1-debian-3.1inch-mipi-20260331.img` | ✅ 3.1" MIPI 版，用这个 |
| `tspi-1m-kernel6.1-debian-hdmi-20260320.img` | HDMI 版，MIPI 屏点不亮 |

> 如果你手上已经有本项目早期版本的自编译 `6.1boot.img`，**不需要再用它**，
> 官方镜像的 boot 分区已经带面板支持。

---

## 2. 烧录

工具：**RKDevTool** + **DriverAssitant**（同在资料包 `第06章` 目录）。

1. 板子断电，USB 线接 **Type-C（OTG）** 口到电脑；
2. 按住 **Recovery/SW** 键，再上电，保持 3 秒后松开；
3. RKDevTool 显示「发现一个 LOADER 设备」；
4. 切到 **「升级固件」** → 固件选 `.img` → **「升级」**；
5. 等待「下载完成」（约 3–6 分钟），板子自动重启。

默认账号 **`linaro` / `linaro`**（root 同密码）。

---

## 3. 官方镜像自身的两个坑

### 3.1 sshd 起不来：没有 host key

现象：SSH 连接被拒，串口里看到

```
sshd: no hostkeys available -- exiting
```

```bash
ssh-keygen -A
systemctl restart ssh
```

### 3.2 机芯网卡没被 NetworkManager 接管

`usb0`（机芯的 NCM 网卡）状态是 `unavailable`，一直不配 IP，
程序就会一直 `frames=0` 连不上机芯。

```bash
nmcli device set usb0 managed yes
nmcli con mod 4117 connection.autoconnect yes
nmcli con up 4117
```

两条 `setup_device.sh` 都会自动处理。

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
3. `apt-get install --no-upgrade` 装依赖（**不升级已装包**，
   否则可能把镜像里打过补丁的 network-manager 顶掉，WiFi 会断）
4. `scripts/fetch_lvgl.sh` 拉 LVGL 9.2.2（GitHub → Gitee 兜底）
5. 编译
6. 写 udev 规则把机芯网卡固定成 `usb0`，建 `192.168.64.32/24` 的 NM 连接并激活
7. 关闭 lightdm、切 `multi-user.target`、装 systemd 服务并启动

跑完屏幕应显示热像画面，日志里能看到：

```
relay: :8081 -> 192.168.64.64:80
rtsp: rtsp 320x240 codec=mjpeg
[..] frames=175 max=27.1 min=22.2 ctr=24.8 focus=57
```

---

## 5. 机芯设置

机芯需要先切到 **NCM 模式**，IP 改为 `192.168.64.64`（出厂多为 UVC 模式或 `10.0.0.x`）。
改法见 [ncm_switch.md](ncm_switch.md)，接口清单见 [4117_isapi_verified.md](4117_isapi_verified.md)。

---

## 6. 屏幕不亮时

```bash
systemctl get-default          # 应为 multi-user.target
pgrep -a lightdm || echo ok    # 不应有输出
ls /sys/class/drm/             # 应能看到 card0-DSI-1
journalctl -u tb4117.gui.service -b -n 40
```

Xorg 占着 DRM 时 `/dev/dri/card0` 被 lightdm 持有，LVGL 无法提交画面。
