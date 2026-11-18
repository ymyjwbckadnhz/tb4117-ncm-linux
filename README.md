# linux-app · 泰山派手持热像仪

泰山派（RK3566）上的原生热成像仪，**LVGL 9 + DRM** 直出 480×800 MIPI 屏，
通过 USB-NCM 走 ISAPI 读取海康 **TB-4117-3/S** 机芯，能拿到**逐像素温度矩阵**。

<!-- 图片走 jsDelivr 绝对地址，GitHub 和 Gitee 两边都能显示 -->
<p>
<img src="https://cdn.jsdelivr.net/gh/ymyjwbckadnhz/tb4117-ncm-linux@master/docs/images/thermal.png" width="240">
<img src="https://cdn.jsdelivr.net/gh/ymyjwbckadnhz/tb4117-ncm-linux@master/docs/images/settings.png" width="240">
<img src="https://cdn.jsdelivr.net/gh/ymyjwbckadnhz/tb4117-ncm-linux@master/docs/images/files.png" width="240">
</p>

---

## 功能

| 功能 | 说明 |
|---|---|
| **实时热图** | RTSP MJPEG 解码 → swscale 直出 480×360 RGB565，25 fps |
| **高低温跟踪** | `rulesTemperatureInfo` 5 Hz，画面标注最高/最低温位置 + 数值 |
| **中心测温** | 温度矩阵中心像素，1 Hz |
| **点测温** | 点画面添加测温点（最多 8 个），可拖动，「清除」删除 |
| **标记开关** | 最高 / 最低 / 中心 三个标记可分别隐藏 |
| **调色板** | 白热 / 黑热 / 铁红1 / 铁红2 / 彩虹 / 融合1 / 融合2 / 棕褐 |
| **拍照** | 480×360 JPEG + 同名 `.json`（记录当时温度快照） |
| **录像** | 直接封装 RTSP 的 MJPEG 帧为 AVI，**不重新编码** |
| **文件管理** | 相册网格（缩略图 + 时间 + 视频角标），可按 全部/照片/视频 筛选，滑动浏览全部 |
| **预览** | 全屏查看，**左右滑动切换**（带滑入动画），可删除 |
| **对焦辅助** | Tenengrad 梯度实时评价清晰度，底部横条 + 85% 参考刻度 |
| **亮度 / 对比度** | `/ISAPI/Image/channels/1/Color`，滑块实时下发 |
| **屏幕亮度** | GP7101（I2C1 @0x58，负极性 PWM） |
| **WiFi** | `nmcli` 扫描 + 密码键盘连接 |
| **网页管理** | 实时 MJPEG 流、状态、拍照/录像、缩略图、批量下载（ZIP）/批量删除 |
| **机芯配置页转发** | :8081 纯 TCP 转发到机芯 Web UI，另转发 554（RTSP）与 8000（私有协议） |
| **开机自启** | systemd 服务，异常自动重启 |

### 界面

底部四个标签切换：

| 页面 | 内容 |
|---|---|
| **热像** | 热图 + 三个标记 + 三测温开关 + 调色板 + 拍照/录像 + 对焦条 + 网页地址 |
| **设置** | 顶部实时预览小窗 + 亮度/对比/屏幕滑块 + 网页地址 + 清除测温点 |
| **网络** | WiFi 状态 + 扫描列表 + 密码弹窗 |
| **文件** | 相册网格 + 筛选 + 预览（左右滑动）+ 删除 |

---

## 硬件与环境

| 部件 | 说明 |
|---|---|
| 主板 | 立创·泰山派 RK3566 1M，Debian 12（kernel 6.1，aarch64） |
| 屏 | 3.1" MIPI DSI，**480×800 竖屏**，DRM `/dev/dri/card0` |
| 触摸 | `/dev/input/event1`，I2C1 @0x38 |
| 背光 | 扩展板 SY7201 + **GP7101**（I2C1 @0x58，8 位 PWM，负极性） |
| 机芯 | 海康 TB-4117-3/S，配成 NCM 网卡 |
| 网络 | 板子 `usb0` = 192.168.64.32，机芯 192.168.64.64 |

机芯访问用 HTTP Digest，**凭据不编进代码**，而是读 `~/linux-app/device.conf`：

```ini
camera=192.168.64.64
auth=admin:你的密码
```

没有任何配置文件时会退回 `admin:admin` 并在日志里提示。
**请先把机芯的出厂密码改掉**，再写进这个文件（`chmod 600`，已在 `.gitignore` 里）。

视频：`rtsp://<auth>@<camera>:554/Streaming/Channels/101`（MJPEG 320×240）。

板子侧配置（udev 固定网卡名、静态 IP、关 lightdm）见 [docs/taishanpi_setup.md](docs/taishanpi_setup.md)，
ISAPI 实测记录见 [docs/4117_isapi_verified.md](docs/4117_isapi_verified.md)。

---

## 系统镜像

**用官方镜像就行，不需要自建或分发任何镜像。** 立创的
`tspi-1m-kernel6.1-debian-3.1inch-mipi-20260331.img`（4.46 GB）自带 3.1" MIPI 面板支持，
本项目就在这套镜像上实测通过。

```
资料下载中心  https://wiki.lckfb.com/zh-hans/tspi-rk3566/download-center.html
百度网盘      https://pan.baidu.com/s/5m1jN3xFWAu_8qbUbidNeMw
路径          第06章.【立创·泰山派】系统镜像 / kernel6.1 / 【镜像】Debian12 /
```

刷完跑 `bash scripts/setup_device.sh` 即可。烧录步骤，以及官方镜像自身的两个坑
（sshd 缺 host key、usb0 未被 NetworkManager 接管）见
**[docs/flash_image.md](docs/flash_image.md)**。

---

## 编译

> 3.1" 屏只有触摸没有键盘，**别在屏上敲命令**。用串口或 SSH 登录，
> 或者在 PC 上跑 `scripts/bootstrap.ps1`（见下文「部署」）。

```bash
git clone https://github.com/ymyjwbckadnhz/tb4117-ncm-linux.git ~/linux-app
cd ~/linux-app
```

依赖：

```bash
sudo apt install build-essential cmake pkg-config libfreetype-dev \
    libcurl4-openssl-dev libjpeg62-turbo-dev libdrm-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev
```

LVGL 9.2.2 不在仓库里，先拉一份：

```bash
bash scripts/fetch_lvgl.sh        # -> third_party/lvgl
```

然后在板子上编译：

```bash
bash scripts/build.sh              # 编译 + 重启服务
bash scripts/build.sh --no-restart # 只编译
```

**在 PC 上开发推荐**一键部署（只推改动文件 + 增量编译，端到端约 8 秒）：

```powershell
.\scripts\deploy.ps1
.\scripts\deploy.ps1 -NoRestart
```

| 场景 | 耗时（RK3566） |
|---|---|
| 无改动 | 1–2 s |
| 只改 `src/main.c` | 6 s |
| 全量（首次 / 改 `lv_conf.h`） | 52 s |

> `CMakeLists.txt` 关掉了 LVGL 的 demos / examples / thorvg，且只构建
> `--target tb4117_verify`，否则每次都会编几百个示例文件。

---

## 运行与部署

### 免编译安装（Releases 里的运行包）

不想装编译环境的话，直接下运行包（436 KB）：

```bash
tar xzf tb4117-ncm-linux-v1.0.0-aarch64.tar.gz
cd tb4117-ncm-linux-v1.0.0-aarch64
bash install.sh
```

包里是已链接 LVGL 的 `tb4117_verify` + `install.sh`（装运行库、拷程序、配 usb0、
装 systemd 服务）。自己打包：`VER=v1.0.0 bash scripts/package.sh`。

### 在电脑上一条命令完成部署（推荐）

板子联网、SSH 通之后，在**电脑**上执行——全程不用在 3.1" 屏上输入任何东西：

```powershell
.\scripts\bootstrap.ps1 -Board linaro@192.168.1.50
```

```bash
BOARD=linaro@192.168.1.50 bash scripts/bootstrap.sh
```

它会：上传源码 → 在板子上跑 `setup_device.sh`（装依赖 + 拉 LVGL + 编译 +
配 `usb0` + 装 systemd 服务 + 关 lightdm）。

### 在板子上直接跑

已经通过串口或 SSH 登录时：

```bash
bash scripts/setup_device.sh
```

### 首次开机怎么进板子

| 方式 | 说明 |
|---|---|
| **串口** | USB-TTL，**1 500 000** 波特率，`linaro`/`linaro`。首次开机没网时唯一入口 |
| **SSH** | `ssh linaro@<板子IP>`（密码 `linaro`）。官方镜像首次开机 sshd 缺 host key，需先用串口执行 `ssh-keygen -A && systemctl restart ssh` |
| **USB 键鼠** | 直接插到板子上，用 3.1" 屏的桌面开终端——**字非常小**，应急可用；有 HDMI 显示器更舒服 |

详见 [docs/flash_image.md](docs/flash_image.md)。

手动方式：

```bash
sudo ./build/tb4117_verify          # 手动跑
sudo bash tools/install_service.sh  # 装成开机自启
sudo journalctl -u tb4117.gui.service -f
```

服务是 `/etc/systemd/system/tb4117.gui.service`（root、`Restart=always`）。
必须关闭 lightdm 并切到 multi-user.target，否则 Xorg 占着 DRM master，屏幕全黑。

### 修改机芯密码

出厂默认密码一定要改。两种方式：

```bash
# 1) 机芯 Web 页（推荐，走本项目的 8081 转发）
#    http://<板子IP>:8081/ → 系统 → 用户管理 → 修改 admin 密码

# 2) ISAPI（PUT /ISAPI/Security/users/1）
curl -u admin:<旧密码> --digest -X PUT \
  -H "Content-Type: application/xml; charset=UTF-8" \
  --data-binary @user.xml http://192.168.64.64/ISAPI/Security/users/1
```

改完写进 `device.conf`，然后 `sudo systemctl restart tb4117.gui.service`。

---

## 架构

```
main（LVGL 线程）
├── lv_timer_handler() + update_ui()     界面刷新
├── video_thread   RTSP → MJPEG 解码 → sws → RGB565(480×360) → 共享帧缓冲
│                  顺带算对焦值、按需写 AVI、给网页流留一份原始帧
├── temp_thread    rulesTemperatureInfo 5 Hz（最高/最低/平均 + 坐标）
├── matrix_thread  jpegPicWithAppendData 1 Hz（160×120 float32 矩阵）
├── color_thread   亮度/对比度下发（120 ms 去抖）
├── http_thread    :8080 网页与接口
├── proxy_thread   :8081→80 / :554→554 / :8000→8000 纯 TCP 转发
└── relay 线程     每个网页流连接、每个转发连接各一个
```

采集线程只写共享结构（互斥量保护），LVGL 线程负责搬运和绘制。
nmcli、拍照、录像等阻塞操作都在工作线程里，不阻塞界面。

---

## 网页接口

浏览器打开 `http://<板子IP>:8080/`。

| 接口 | 说明 |
|---|---|
| `GET /` | 管理页面 |
| `GET /stream.mjpg` | MJPEG 实时流（25 fps） |
| `GET /preview.jpg` | 单帧 JPEG（缓存 200 ms） |
| `GET /status` | `{"recording":0,"frames":115,"files":5,"focus":255}` |
| `GET /files.json` | 文件列表 |
| `GET /poster?f=` | 视频首帧封面 |
| `GET /zip?f=A&f=B` | 打包下载 |
| `GET /files/<名字>` | 下载文件 |
| `GET /delete?f=` | 删除（照片会连带删 .json） |
| `GET /capture` · `/record/start` · `/record/stop` | 拍照 / 录像 |
| `GET /color?b=60&c=70` · `/screen?b=50` | 调图像 / 屏幕亮度 |
| `GET /wifi/scan` · `/wifi/status` | WiFi |
| `GET /preview?f=` · `/page?p=0..3` · `/slide?d=1` | 调试用：开预览、切页、触发滑动 |

文件落在 `~/linux-app/media/`：

- `photo_YYYYmmdd_HHMMSS.jpg` — 480×360 热图（不含 UI 覆盖层）
- `photo_YYYYmmdd_HHMMSS.json` — `{"max":…,"min":…,"avg":…,"center":…}`
- `video_YYYYmmdd_HHMMSS.avi` — 320×240 MJPEG 25 fps，VLC 可直接播

机芯配置页与端口转发：

| 板子端口 | 机芯端口 | 用途 |
|---|---|---|
| 8081 | 80 | 机芯 Web 配置页 |
| 554 | 554 | RTSP：`rtsp://<板子IP>:554/Streaming/Channels/101` |
| 8000 | 8000 | 海康私有协议 |

---

## 启动时间

断电冷启动（串口实测，从 DDR 训练算起）：

| 阶段 | 本段 | 累计 |
|---|---|---|
| 上电 / DDR 训练 | 0.44 s | 0.44 s |
| U-Boot | **4.22 s** | 4.65 s |
| 内核 → systemd | 2.40 s | 7.06 s |
| systemd → 应用起来 | 4.59 s | **11.64 s** |
| 机芯网卡就绪（画面接上） | 4.79 s | 16.43 s |

复位重启更快：内核 4.1 s 后就出画面，因为机芯没断电、网卡早就在了。

### 官方镜像自带的坑

`/etc/network/interfaces.d/network-priority` 里 auto 了 `eth0` / `wlan0` / `ec20`，
这三个在这块板子上都不存在（真正用的是 `usb0` + `wlan1`），于是每次开机
`ifupdown` 都要跑一遍 DHCP 等超时，`networking.service` 最后以失败告终，
能把 multi-user.target 拖到几分钟。`setup_device.sh` 会把这三行 `auto` 注释掉。

> 备份文件**不要**放在 `/etc/network/interfaces.d/` 里：那个目录是
> `source /etc/network/interfaces.d/*` 全量加载的，`.orig` 也会被读进去。

另外 systemd 单元不能写 `After=multi-user.target`——界面只要 DRM + 输入设备，
等整个 multi-user 就等于把这堆网络超时全等一遍。

### 还能再快吗

U-Boot 占 4.2 s，主要是 DDR 从 324 MHz 升到 1056 MHz 的训练。想压只能改 U-Boot 的
DDR 参数/去掉无用外设探测，收益有限，没动。

---

## 若干实现细节（踩过的坑）

### 温度矩阵

`jpegPicWithAppendData?format=json` 返回 multipart：JSON + JPEG +
**76800 B = 160×120 个 float32**，追加段就是逐像素温度，点测温直接查表。

### 录像零编码

RTSP 源就是 MJPEG，把 `AVPacket` 原样写进自己实现的 AVI（RIFF）容器，
不解码不编码，录像时 CPU 几乎不增加。

### 网页实时流

用 **MJPEG 长连接**（`multipart/x-mixed-replace`）而不是定时刷 `<img>`：
轮询每条请求都要建连 + 编码，且我们的 HTTP 服务是单线程的，会被堵住。

转发的是**机芯原始 JPEG 帧**（双缓冲 + 版本号），不重新编码——
第一版按 320×240 编码推流，25 fps 直接吃满一个 CPU 核（100%），改成转发后只多 6%。

实测：24.4 fps，329 KB/s。

### 背光 GP7101

面板背光不走 SoC 的 pwm5，而是扩展板 SY7201 + GP7101（I2C 转 PWM）。
驱动接的是**负极性**输出，占空比要镜像：

```c
int raw = (100 - pct) * 255 / 100;   /* 值越大越暗 */
uint8_t buf[2] = { 0x03, raw };      /* 寄存器 0x03 = 8 位 PWM */
```

### 字体

界面统一用 **FreeType 加载系统 TTF**（文泉驿正黑，全 CJK），中英文同一字库，
基线天然对齐；字号 26（主）/ 20（次）。

找不到 TTF 时退回编译进二进制的 `src/lv_font_cn_22.c`
（只含源码里出现的汉字，用 `python tools/gen_font.py` 重新生成）。

### LVGL 时钟源

`lv_linux_drm` **不会**设置 tick，不调用 `lv_tick_set_cb()` 的话
`lv_tick_get()` 恒为 0，所有动画和定时器都冻在起点：

```c
static uint32_t lv_tick_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000u + ts.tv_nsec / 1000000u;
}
lv_init();
lv_tick_set_cb(lv_tick_ms);
```

### 机芯 Web 页转发

机芯的登录是挑战-响应会话，**HTTP 层代理会打断会话绑定**（会报"网络异常"）。
改成**纯 TCP 转发**（每连接一个线程，`select()` 双向搬字节）后，浏览器和机芯就是直连，
Cookie、Digest、chunked 全部端到端正确。

### 其他

- 弹窗打开时冻结热图重绘，否则 25 fps 的画面会把弹窗刷花；
- 设置页预览是把 480×360 抽稀成 320×240 的独立缓冲，显示时不缩放（缩放变换会撕裂）；
- 预览视频限流 10 fps，避免局部重绘被拆成多段产生接缝。

---

## 机芯复位与自愈

**断电时间不够会导致机芯"半复位"**：它的网络栈活过来了（ping、80 端口都通），
但 RTSP 服务始终不启动，于是界面一直停在"机芯启动中…"。

实测 6 次上电里 2 次踩中：

| 上电 | RTSP 就绪 |
|---|---|
| 3 次 | up=31~32 秒 |
| **2 次** | **从未就绪**（半复位） |

**正确做法：断电后等 10~15 秒再上电**，让机芯的储能放完。

### 自动自愈

`tb4117-probe.service` 每 2 秒采样 ping / 80 / 554。
如果 **ping 和 80 都通、但 554 连续 150 秒不通**，就通过 ISAPI 给机芯下发重启
（`PUT /ISAPI/System/reboot`），10 分钟内最多一次。

```
stuck: http up but rtsp down for 152s -> rebooting module
SELF-HEAL: PUT /ISAPI/System/reboot -> 200
MODULE READY after 268s board uptime
```

不需要人工干预，界面上的计时会继续走，连上后自动出画面。

### 手动排查

```bash
sudo tail -f /var/log/tb4117/probe.log     # 跨重启保留，1 MB 轮转
python3 tools/analyze_probe.py             # 按启动分组汇总
bash tools/module_reboot.sh                # 手动重启机芯并等待 RTSP
```

日志每次开机都会记录 USB 枚举行、机芯 IP、以及机芯自己的时钟（它没有 RTC，
Date 头就是它的开机时长），所以能区分"服务没起来"和"机芯在反复重启"。

---

## 已知限制

| 限制 | 说明 |
|---|---|
| 点测温 1 Hz | 与中心测温共用温度矩阵 |
| 照片不含标注 | 存纯热图，温度在同名 json |
| 录像不含 UI | 只录机芯原始画面 |
| 测温点不持久化 | 重启清空 |
| 预览视频 10 fps | 限流换取无撕裂 |
| 机芯自带预览不可用 | 它用 ActiveX 插件，现代浏览器不支持；用 :8080 的流代替 |

---

## 目录

```
linux-app/
├── src/
│   ├── main.c             全部逻辑（单文件）
│   └── lv_font_cn_22.c    中文字体 fallback
├── scripts/
│   ├── fetch_lvgl.sh      下载 LVGL 9.2.2
│   ├── build.sh           板子上编译 + 重启
│   └── deploy.ps1         PC 上一键部署
├── tools/
│   ├── gen_font.py        扫描源码汉字生成字体子集
│   ├── grab.py            抓屏存 PNG
│   ├── drm_grab.c         直接读 DRM plane 截图
│   ├── diag_display.sh    黑屏排查
│   ├── install_service.sh 安装 systemd 服务
│   └── run_app.sh         前台运行
├── docs/                  板子配置、ISAPI 实测、界面截图
├── lv_conf.h
├── CMakeLists.txt
└── third_party/lvgl       LVGL 9.2.2（用 scripts/fetch_lvgl.sh 拉取）
```

---

## 许可证

Apache-2.0，见 [LICENSE](LICENSE)。

**非官方项目**，与海康威视及任何模组厂商无关。
