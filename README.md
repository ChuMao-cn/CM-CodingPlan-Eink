# CM-CodingPlan-Eink

ESP32 + 1.54 寸墨水屏的火山方舟 Coding Plan 用量看板。本地服务读取 Chrome 中的 Coding Plan 页面数据，ESP32 通过局域网定时拉取 JSON 并刷新墨水屏。

## 架构

```text
Chrome Coding Plan 页面
  → macOS AppleScript 抓取页面文本
  → Python HTTP 服务解析并缓存
  → ESP32 每 10 分钟请求 /display.json
  → SSD1681 1.54 寸墨水屏渲染
```

数据接口：

```http
GET /display.json
```

返回字段：

```json
{
  "five_hour": "41.5%",
  "one_week": "5.8%",
  "one_month": "25.49%",
  "reset": "1234分钟",
  "updated": "11:20"
}
```

## 硬件

- ESP32-DevKitC v4
- 1.54 寸黑白墨水屏，200×200，SSD1681
- SPI 与控制引脚：SCLK 13、MOSI 14、CS 15、D/C 27、RST 26、BUSY 25

## 服务端

服务端只运行在 macOS 上，依赖 Chrome AppleScript 抓取。使用前：

1. Chrome 保持打开火山方舟 Coding Plan 页面。
2. 开启 Chrome 菜单：`查看 > 开发者 > 允许 JavaScript 来自 Apple 事件`。
3. 在项目根目录启动服务：

```bash
python3 server/server.py
```

服务监听 `0.0.0.0:8000`。页面未打开或抓取失败时，返回最近一次成功抓取的数据。`server.log` 记录 ESP32 的请求日志。

## 固件

固件使用 ESP-IDF v5.5.5。macOS 已通过 EIM 安装工具链时，先激活：

```bash
source /Users/cm/.espressif/tools/activate_idf_v5.5.5.sh
```

进入固件目录：

```bash
cd firmware/coding-plan-epd
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p /dev/cu.usbserial-1130 flash monitor
```

在 `Coding Plan Display` 中配置：

- `ESP_WIFI_SSID`
- `ESP_WIFI_PASSWORD`
- `DATA_URL`，例如 `http://192.168.1.100:8000/display.json`

`sdkconfig` 包含 WiFi 密码，已被 `.gitignore` 排除，不要提交。

## 运行逻辑

- 北京时间 `10:00–19:00` 保持在线，每 10 分钟抓取并刷新一次。
- 北京时间 `19:00–10:00` 深度休眠，一次睡到次日 10:00。
- 唤醒后先 NTP 对时，再判断休眠窗口。
- WiFi 最多重试 3 次。
- WiFi、抓取或解析失败时保留墨水屏旧画面。
- ESP32 启动、NTP、抓取、WiFi 重试、休眠和刷新结果都会输出 ESP-IDF 日志。

## 目录

```text
server/                 macOS 抓取服务和 HTTP 接口
firmware/coding-plan-epd/  ESP-IDF 固件
mock.html/              200×200 墨水屏布局模拟页
tools/                  本地 ESP-IDF/EIM 工具链，不入库
```
