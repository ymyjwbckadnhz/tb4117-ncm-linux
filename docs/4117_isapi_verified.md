# TB-4117-3/S ISAPI 实测记录

> 设备：`TB-4117-3/S`，固件 `V5.5.10 build 210208`，序列号 `...F66490930`
> 网络：**192.168.64.64/24 静态（USB-NCM 链路）**，2026-11 由 WEB 把网络模式改为 NCM
> 板子侧：`usb0` = 192.168.64.32/24（NetworkManager 连接 `4117`），详见 taishanpi_setup.md
> 认证：HTTP Digest，`admin` / 见密码本

## 1. 已验证可用的接口

### 设备信息

```bash
curl -u admin:*** --digest http://192.168.64.64/ISAPI/System/deviceInfo
```

### 实时温度（最高/最低/平均 + 坐标）

```bash
curl -u admin:*** --digest \
  "http://192.168.64.64/ISAPI/Thermal/channels/1/thermometry/1/rulesTemperatureInfo?format=json"
```

```json
{"maxTemperature":21.2,"minTemperature":12.7,"averageTemperature":19.5,
 "MaxTemperaturePoint":{"positionX":0.994,"positionY":1.000},
 "MinTemperaturePoint":{"positionX":0.094,"positionY":0.084}}
```

坐标为 **0~1 归一化**，乘宽高即得像素位置。

### 温度矩阵 + JPEG（关键接口）

```bash
curl -u admin:*** --digest -o out.bin \
  "http://192.168.64.64/ISAPI/Thermal/channels/1/thermometry/jpegPicWithAppendData?format=json"
```

返回 `multipart/form-data`，共三段：

| 段 | Content-Type | 长度 | 内容 |
|---|---|---|---|
| 1 | application/json | 201 B | 元数据（见下） |
| 2 | image/pjpeg | 16332 B | 160×120 JPEG |
| 3 | application/octet-stream | **76800 B** | 160×120 **float32 温度矩阵**（小端，℃） |

元数据段：

```json
{"JpegPictureWithAppendData":{"channel":1,"jpegPicLen":16332,
 "jpegPicWidth":160,"jpegPicHeight":120,"p2pDataLen":76800,
 "isFreezedata":false,"temperatureDataLength":4}}
```

解析要点：
- 三段用 `--boundary` 分隔，每段头尾各有一个 `\r\n\r\n`；
- 矩阵按行优先，`index = y*160 + x`，`struct.unpack('<19200f', data)`；
- 实测 min 13.76 / max 21.15 ℃，与 `rulesTemperatureInfo` 一致。

### 调色板

```bash
# 读
curl -u admin:*** --digest http://192.168.64.64/ISAPI/Image/channels/1/Palettes
# 写（注意：Content-Type 必须带 charset，否则报 badXmlFormat）
curl -u admin:*** --digest -X PUT \
  -H "Content-Type: application/xml; charset=UTF-8" \
  --data-binary @pal.xml \
  http://192.168.64.64/ISAPI/Image/channels/1/Palettes
```

`pal.xml`：

```xml
<?xml version="1.0" encoding="UTF-8"?>
<Palettes version="2.0" xmlns="http://www.std-cgi.com/ver20/XMLSchema"><mode>Ironbow1</mode></Palettes>
```

可选值（来自 LiThermal）：`WhiteHot` `BlackHot` `Fusion1` `Fusion2` `Rainbow` `Ironbow1` `Ironbow2` `Sepia`

### 亮度 / 对比度（Color）

`/ISAPI/Image/channels/1/brightness` 和 `/contrast` 都是 **403**（不存在），
真正的字段藏在 ImageChannel 资源的 `Color` 块里：

```bash
# 读
curl -u admin:*** --digest http://192.168.64.64/ISAPI/Image/channels/1/Color
```

```xml
<Color version="2.0" xmlns="http://www.std-cgi.com/ver20/XMLSchema">
  <brightnessLevel>43</brightnessLevel>
  <contrastLevel>43</contrastLevel>
</Color>
```

```bash
# 写（同样要 charset）
curl -u admin:*** --digest -X PUT \
  -H "Content-Type: application/xml; charset=UTF-8" \
  --data-binary @color.xml \
  http://192.168.64.64/ISAPI/Image/channels/1/Color
```

取值范围实测 **1~100**（默认 43），写完立即生效并回读一致。

### 测温参数

```bash
GET  /ISAPI/Thermal/channels/1/thermometry/basicParam
PUT  /ISAPI/Thermal/channels/1/thermometry/basicParam   (同样要 charset)
GET  /ISAPI/Thermal/channels/1/thermometry/basicParam/capabilities
```

字段：`enabled`、`streamOverlay`（设备自绘 OSD 开关）、`emissivity`、`distance`、`temperatureUnit`、`displayMax/Min/AverageTemperatureEnabled`。

### 视频

```bash
# RTSP（MJPEG）
rtsp://admin:***@192.168.64.64:554/Streaming/Channels/101
# 单帧快照
curl -u admin:*** --digest -o snap.jpg http://192.168.64.64/ISAPI/Streaming/channels/101/picture
```

分辨率固定 **320×240 MJPEG**（capabilities 里只有这一档）。

## 2. 当前配置（2026-11 实测）

| 项 | 值 |
|---|---|
| 调色板 | `Ironbow1`（原 `Fusion1`） |
| `streamOverlay`（设备 OSD 文字） | **`false`**（原 `true`）——数值交给 App 自绘 |
| `enabled`（测温） | `true` |
| 发射率 / 距离 | `0.98` / `100 cm` |
| 温度范围 | `30~45`（固件固定，capabilities 只有这一档） |
| max/min/avg OSD | 全部 `false` |
| 网络 | DHCP，`192.168.64.64` |

**备份**：`demo/basicParam_backup.xml`（改前原样）。恢复：

```bash
curl -u admin:*** --digest -X PUT -H "Content-Type: application/xml; charset=UTF-8" \
  --data-binary @basicParam_backup.xml \
  http://192.168.64.64/ISAPI/Thermal/channels/1/thermometry/basicParam
```

## 3. 注意事项

1. **温度范围 30~45℃**：capabilities 只提供这一档，说明是体温型模组。实测能读到 12.7℃ 和 21.2℃，低于 30 也能测，但**高温物体（>45℃）可能超量程**，测 PCB 发热点要留意。
2. **PUT 必须带 `charset=UTF-8`**，否则返回 `badXmlFormat`（statusCode 5）。
3. 文件不要带 UTF-8 BOM（PowerShell 的 `Set-Content -Encoding UTF8` 会加 BOM，用 `[IO.File]::WriteAllText` + `UTF8Encoding($false)`）。
4. `httpPreview` 走 MJPEG 分块流，拉一帧不方便；**抓图用 RTSP + ffmpeg** 或 `/picture`。
5. 若将来机芯直连板子（USB-NCM），需要把网络改成固定 IP（例如 `192.168.64.64/24`），主机侧 `ifconfig usb0 192.168.64.32`。
