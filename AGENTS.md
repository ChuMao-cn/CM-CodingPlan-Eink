# CM-CodingPlan-Eink

## 用途
读取火山方舟 Coding Plan 用量，并通过局域网接口刷新 1.54 寸 ESP32 墨水屏。

## 技术栈
- ESP32 + 1.54 寸墨水屏
- 本地 HTTP 服务
- 前端先用 HTML/CSS 做墨水屏效果模拟

## 目录
- `mock.html`：1.54 寸墨水屏前端模拟页，使用 200x200 视口
- `tools/esp-idf-eim/v5.5.5/esp-idf`：ESP-IDF v5.5.5
- `tools/eim/eim`：Espressif EIM CLI
- `firmware/coding-plan-epd`：WiFi 拉取 `/display.json` 并刷新 1.54 寸墨水屏的固件
- `server`：临时静态 `/display.json` 服务，端口 8000

## 约定
- ESP32 只负责请求 `/display.json` 并渲染，不直接抓火山方舟页面。
- 数据源优先用页面隐藏接口；不可用时再考虑浏览器自动化。
- 墨水屏刷新频率保持 10 分钟级别，避免频繁刷新影响寿命。
- 固件期望 `/display.json` 返回 `five_hour`、`one_week`、`one_month`、`reset` 四个字符串字段。
- 编译命令：在 `firmware/coding-plan-epd` 执行 `idf.py build`。
- 临时服务启动命令：在项目根目录执行 `python3 server/server.py`。

## 当前需求
功能已跑通；仓库文档化后发布到 GitHub 私有仓库。

## 已确认
- 火山方舟没有开放额度查询接口。
- Coding Plan 页面可直接读取：近 5 小时 / 近一周 / 近一月用量、重置时间和套餐到期时间。
- 模拟页右上角显示 Coding Plan。
- 隐藏接口：`POST https://ml-platform-api.console.volcengine.com/ark/bff/api/GetCodingPlanUsage`。
- 该接口需要浏览器登录态和 `csrfToken` Cookie；无登录态时返回 `InvalidCSRFToken`。
- 真实抓取采用 Chrome AppleScript + 页面 `innerText`；需在 Chrome 开启“查看 > 开发者 > 允许 Apple 事件中的 JavaScript”，并保持 Coding Plan 页面打开。
- Chrome/页面未运行时，服务返回最近一次成功抓取的数据。
- 固件采用深度休眠：19:00–10:00 一次休眠到 10:00；10:00–19:00 保持在线并每 10 分钟抓取刷新。
- WiFi 最多重试 3 次；抓取或解析失败时保留上一次墨水屏画面。
- 墨水屏刷新完成信号量只在 `wait_epaper_done()` 中等待；`epaper_show()` 前不能重复 take，否则第二轮会卡在刷新前。
- 硬件：ESP32-DevKitC v4 + 1.54 寸黑白墨水屏（200×200，SSD1681）。
- 引脚：SCLK 13、MOSI 14、CS 15、D/C 27、RST 26、BUSY 25。
- 开发环境：Windows ESP-IDF v5.5.5；原固件工程 `C:\Espressif\projects\epd_weather`，串口 COM3。
- macOS 当前识别到串口 `/dev/cu.usbserial-1130`。
- macOS 环境已通过 EIM 安装 ESP-IDF v5.5.5。
- 终端激活命令：`source /Users/cm/.espressif/tools/activate_idf_v5.5.5.sh`。
- `CONFIG_DATA_URL` 必须只填完整 URL，不能包含 menuconfig 提示文本。
- `/display.json` 必须返回 `Content-Length`；否则 ESP-IDF 会按 chunked 响应处理并报 `Incomplete chunked data received`。
- 当前 mock 服务运行在 `http://10.10.14.58:8000/display.json`，ESP32 IP 为 `10.10.14.16`。
- 设备已能成功请求 mock JSON，串口无 HTTP 错误。
- 固件自定义 5×7 字体曾缺 `I`、`L`、`A`，导致 `CODING PLAN` 缺字；已补齐。
- ESP-IDF 中 `esp_http_client_perform()` 会消耗响应体；需要保留 JSON 时用 `open()` / `fetch_headers()` / `read_response()` / `close()`。
- 自定义字体已补齐 `G`。
- 墨水屏布局按 `mock.html` 调整：三行“周期标签 + 右对齐用量 + 进度条”，底部显示重置时间。
- 墨水屏固件不显示 logo；`CODING PLAN` 标题加粗，显示服务端返回的更新时间。
- 墨水屏底部文案为“xx分钟后重置”；mock 当前快照为 `41.5% / 5.8% / 25.49%`。
- 官方文档推荐 macOS 使用 EIM，不再优先手工 `git clone --recursive`。
