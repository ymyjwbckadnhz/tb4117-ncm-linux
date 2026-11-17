# 泰山派 + Debain + 4117(NCM) 环境配置

> 板子：泰山派 RK3566，Debian，`linaro/linaro`
> 系统：aarch64，Python 3.11.2，NetworkManager 管理网络
> 板子 WiFi：`10.0.0.47/24`（SSID `OxO_Wi-Fi5`）
> 机芯：USB-NCM，`192.168.64.64/24`

## 1. 接口命名（必须做，否则每次重启名字都变）

机芯的 NCM 网卡 MAC 每次上电都不一样，默认会得到 `enx<mac>` 这种名字，重启就变。用 udev 固定成 `usb0`：

```bash
echo 'SUBSYSTEM=="net", ACTION=="add", DRIVERS=="cdc_ncm", NAME="usb0"' \
  | sudo tee /etc/udev/rules.d/70-4117-ncm.rules
sudo udevadm control --reload-rules
```

## 2. 静态地址（NetworkManager）

```bash
sudo nmcli con add type ethernet ifname usb0 con-name 4117 \
  ipv4.method manual ipv4.addresses 192.168.64.32/24 \
  ipv4.never-default yes ipv6.method disabled
```

- `ipv4.never-default yes`：**关键**，否则这条链路的默认路由会盖过 WiFi，导致板子上不了网；
- 机芯侧固定 `192.168.64.64/24`，同网段直连，不需要网关。

验证：

```bash
ping -c2 192.168.64.64
curl -s -u admin:*** --digest http://192.168.64.64/ISAPI/System/deviceInfo
```

## 3. 无线网络

```bash
nmcli device wifi connect 'OxO_Wi-Fi5' password '********'
```

注意 SSID 是 **`OxO_Wi-Fi5`**，不是 `OxO`。

## 4. SSH

```bash
# PC 侧生成密钥并写入板子
ssh-keygen -t ed25519 -f ~/.ssh/id_ed25519_board -N ""
# 板子上：
mkdir -p ~/.ssh && chmod 700 ~/.ssh
echo "<公钥>" >> ~/.ssh/authorized_keys && chmod 600 ~/.ssh/authorized_keys
```

之后：

```bash
ssh -i ~/.ssh/id_ed25519_board linaro@10.0.0.47
```

## 5. 串口（备用）

- 调试串口：`COM10`，**1500000** 波特率，root 已登录
- 工具脚本：`tools/serial.ps1 -Cmd "<命令>"`

## 6. 实测性能（NCM 链路）

| 操作 | 数据量 | 耗时 |
|---|---|---|
| `jpegPicWithAppendData`（JPEG + 温度矩阵） | ~92 KB | **263~281 ms** |
| `rulesTemperatureInfo`（实时温度 JSON） | ~0.4 KB | **139~155 ms** |
| `/Streaming/channels/101/picture`（抓图） | ~4 KB | 256~274 ms |

结论：
- 温度矩阵最高 **~3.5 Hz**，建议 1~2 Hz；
- 实时温度 ~6 Hz 上限，建议 5 Hz；
- 单次请求有 **~140 ms 固定开销**（HTTP + Digest 握手），所以别用高频轮询小接口，尽量合并请求。

## 7. 屏幕与背光

### 显示

3.1" MIPI DSI 面板，**480×800 竖屏**，走 DRM（`/dev/dri/card0`），没有 `/dev/fb0`。

跑图形程序前必须让 Xorg 退出，否则它占着 DRM master，屏幕全黑：

```bash
sudo systemctl disable lightdm
sudo systemctl set-default multi-user.target
```

程序侧 `lv_linux_drm_set_file(disp, "/dev/dri/card0", -1)` 是**必须**调用的，
否则 LVGL 会停留在默认 800×480，画面不提交。

### 背光：GP7101，不是 SoC PWM

设备树里 `backlight` 节点绑的是 `pwm5`（GPIO0_C4 / `fe6e0010.pwm`），但**没有接到面板**——
扩展板用 4 个 0Ω 电阻在"泰山派背光"和"板载背光"之间选择，默认贴的是板载那路：

```
SY7201 升压恒流（20 mA） ← DIM ← GP7101（I2C 转 PWM）
```

- 总线：**i2c-1**（和触摸共用），地址 **0x58**（手册写 0xB0，含读写位）
- 8 位模式：写 **寄存器 0x03**，值 0~255（16 位模式是 0x02，用不上）
- **负极性**：LED 驱动接的是反相输出，值越大越暗

```bash
sudo i2cset -y 1 0x58 0x03 0x4D   # 约 30% 亮度
sudo i2cget -y 1 0x58 0x03        # 回读 0x4d
```

验证设备树确实在驱动 pwm5（但屏幕不会变，因为没接线）：

```bash
sudo cat /sys/kernel/debug/pwm | grep -A1 backlight
# pwm-0 (backlight): requested enabled period: 25000 ns duty: xxxx ns
```

## 8. 踩过的坑

| 现象 | 原因 | 解决 |
|---|---|---|
| 机芯在 `192.168.64.0/24` 扫不到 | 它保留了旧配置的 `10.0.0.3` | 在 WEB/ISAPI 里改地址后再重启 |
| 板子能 ping 通 PC，PC 却连不上板子 | 我在 NCM 上配了 `10.0.0.28/24`，和 PC 撞地址，`/24` 路由压住了 WiFi | 给机芯换成独立网段 `192.168.64.0/24` |
| 重启后 SSH 断 | WiFi 连接已保存，会自动重连 | 无需处理 |
| 板子自带网口 `end1` 报 `DMA engine initialization failed` | 该 PHY 未接/驱动问题 | 不影响使用，走 WiFi + NCM |
