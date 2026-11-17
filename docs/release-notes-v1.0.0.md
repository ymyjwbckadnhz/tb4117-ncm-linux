# v1.0.0 — TB-4117 手持热像仪（泰山派 RK3566）

海康 TB-4117-3/S 热成像机芯的手持机方案：**LVGL 9 + DRM** 直接驱动 480×800 MIPI 屏，
通过 USB-NCM 走 ISAPI 读取机芯，能拿到**逐像素温度矩阵**。

## 功能

- 25 fps 实时热图（RTSP MJPEG → 480×360 RGB565）
- 最高/最低温跟踪（5 Hz）+ 中心测温（1 Hz）
- 点测温：点画面加点、拖动、清除
- 8 种调色板；亮度/对比度（ISAPI Color）；屏幕背光（GP7101，负极性 PWM）
- 拍照（JPEG + 温度 json）、录像（MJPEG AVI，零编码 CPU）
- 相册：缩略图网格、按 全部/照片/视频 筛选、左右滑动预览
- 对焦辅助（Tenengrad + 85% 参考刻度）
- WiFi 扫描与连接（nmcli + 触摸键盘）
- 网页端：25 fps MJPEG 流、拍照/录像、缩略图、批量下载（ZIP）、批量删除
- `:8081` 纯 TCP 转发机芯 Web 配置页，`:554` RTSP，`:8000` 私有协议

## 下载

| 文件 | 大小 | 说明 |
|---|---|---|
| `tb4117-ncm-linux-v1.0.0-aarch64.tar.gz` | 436 KB | **免编译运行包**，含 install.sh |
| `Source code (zip/tar.gz)` | — | 源码，GitHub 自动生成 |

运行包内容：

```
tb4117_verify            已链接 LVGL 的 aarch64 可执行文件
install.sh               装依赖 + 拷程序 + 配网络 + 装服务
tb4117.gui.service       systemd 单元
70-4117-ncm.rules        把机芯网卡固定成 usb0 的 udev 规则
README.txt / README.md / LICENSE
```

## 安装

**系统**：立创官方 `tspi-1m-kernel6.1-debian-3.1inch-mipi-*.img`
（<https://wiki.lckfb.com/zh-hans/tspi-rk3566/download-center.html>），刷好后上电。

**免编译安装**（推荐，板子上执行）：

```bash
tar xzf tb4117-ncm-linux-v1.0.0-aarch64.tar.gz
cd tb4117-ncm-linux-v1.0.0-aarch64
bash install.sh
```

**从源码编译**：

```bash
git clone https://github.com/ymyjwbckadnhz/tb4117-ncm-linux.git ~/linux-app
cd ~/linux-app && bash scripts/setup_device.sh
```

3.1" 屏没有键盘，两种方式取一条：插 USB 键盘直接用 3.1" 屏操作（字很小），
或用串口（**1 500 000** 波特率）/ SSH。

## 已知限制

- 点测温 1 Hz（与中心测温共用矩阵）
- 照片不含标注，温度值在同名 json
- 录像只录机芯原始画面
- 机芯自带 Web 预览需要 ActiveX 插件，现代浏览器不支持；用 `:8080` 的流代替

## 校验

```bash
sha256sum tb4117-ncm-linux-v1.0.0-aarch64.tar.gz
# 见同目录的 .sha256 文件
```

许可证 Apache-2.0，非官方项目，与海康威视无关。
