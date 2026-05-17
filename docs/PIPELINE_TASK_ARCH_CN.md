# 流水线任务架构说明

## 1. 核心任务（按职责分层）

当前工程的任务不是单一流水线，而是“启动流水线 + 功能子流水线”并行组织。核心任务至少包括以下 7 个：

1. `power_music_task`（开机音频）
- 位置：`main/esp32_s3_szp.c:931`
- 职责：播放开机 PCM，播放完成后发事件位 `START_MUSIC_COMPLETED`。

2. `main_page_task`（主界面切换）
- 位置：`main/main.c:35`
- 职责：阻塞等待 `START_MUSIC_COMPLETED`，再进入 `lv_main_page()`。

3. `task_process_att`（姿态传感器应用）
- 位置：`main/app_ui.c:120`
- 职责：初始化 QMI8658，创建姿态 UI，定时器周期更新姿态数据显示。

4. `task_process_sdcard`（TF 卡应用）
- 位置：`main/app_ui.c:815`
- 职责：挂载 SD 卡、扫描文件系统、把结果填充到 LVGL 列表。

5. `task_process_camera`（摄像头应用，当前实际使用路径）
- 位置：`main/app_ui.c:924`
- 职责：循环取帧 `esp_camera_fb_get()`，更新 `lv_img` 数据源并显示。

6. `app_wifi_connect` + `wifi_connect`（WiFi 扫描/连接）
- 位置：`main/app_ui.c:1592`、`main/app_ui.c:1495`
- 职责：前者扫描 AP 并构建列表；后者消费账号队列并执行连接状态机。

7. `get_time_task`（联网后时间同步）
- 位置：`main/app_ui.c:1365`
- 职责：等待 WiFi 连接位，SNTP 同步成功后更新主界面日期/时间。

补充：底层还实现了一条“纯视频双任务流水线” `task_process_camera + task_process_lcd`（`main/esp32_s3_szp.c:585`、`main/esp32_s3_szp.c:570`），通过帧队列直驱 LCD，但当前代码中未被上层调用。

## 2. 任务之间怎么通信（不止消息队列）

### 2.1 EventGroup（阶段同步 / 状态广播）

1. 开机阶段同步
- `main/main.c:64` 创建 `my_event_group`
- `power_music_task` 播放完成后 `xEventGroupSetBits(..., START_MUSIC_COMPLETED)`（`main/esp32_s3_szp.c:964`）
- `main_page_task` 用 `xEventGroupWaitBits` 阻塞等待（`main/main.c:38`）

2. WiFi 状态同步
- `s_wifi_event_group` 创建于 `wifi_scan`（`main/app_ui.c:1442`）
- `event_handler` 在 `STA_START / GOT_IP / DISCONNECTED` 分别置位（`main/app_ui.c:1416-1431`）
- `wifi_connect` 通过 `xEventGroupWaitBits` 等待 `WIFI_CONNECTED_BIT | WIFI_FAIL_BIT`（`main/app_ui.c:1521`）
- `get_time_task` 再次等待 `WIFI_CONNECTED_BIT`（`main/app_ui.c:1367`）

### 2.2 Queue（参数传递 + 控制信号）

1. WiFi 账号队列
- `xQueueWifiAccount = xQueueCreate(2, sizeof(wifi_account_t))`（`main/app_ui.c:1639`）
- 生产者：UI 按键事件中 `xQueueSend` 发送 `{ssid,password,back_flag}`（`main/app_ui.c:1161`、`1584`）
- 消费者：`wifi_connect` 里 `xQueueReceive`（`main/app_ui.c:1502`）
- 额外用途：`back_flag=1` 作为“退出任务”控制消息，而不是另开控制通道。

2. 视频帧队列（预留路径）
- `xQueueLCDFrame = xQueueCreate(2, sizeof(camera_fb_t *))`（`main/esp32_s3_szp.c:599`）
- 摄像头任务生产帧指针（`main/esp32_s3_szp.c:591`）
- LCD 任务消费并显示后归还帧缓冲（`main/esp32_s3_szp.c:576-579`）

### 2.3 共享状态变量（轻量控制）

1. `icon_flag`
- 用于控制“当前应用上下文”和任务退出条件（例如摄像头任务 `while (icon_flag == 4)`，见 `main/app_ui.c:926`）。
- 退出按钮回调只改 flag，不直接杀线程，任务自然收敛退出。

2. `camera_lcd_flag`
- 底层双任务视频链路的运行开关（`main/esp32_s3_szp.c:568`）。

### 2.4 LVGL 线程安全锁

- 多任务都可能修改 UI，因此大量路径在 UI 更新前后使用 `lvgl_port_lock/unlock`（如 `main/app_ui.c:125`、`822`、`1386`、`1531`）。
- 这实际上是“渲染临界区协议”，防止并发改 UI 导致的崩溃和显示异常。

## 3. 数据流：摄像头 -> 显示 -> UI

当前工程里有两条路径。

### 3.1 当前正在使用的路径（Camera App 页面）

1. 用户点击主页面摄像头图标
- 入口：`camera_event_handler`（`main/app_ui.c:948`）
- 动作：初始化摄像头 `bsp_camera_init()`（`main/app_ui.c:950`），创建 `lv_img` 控件（`main/app_ui.c:965`），启动摄像头任务（`main/app_ui.c:987`）

2. 摄像头任务取帧
- `frame = esp_camera_fb_get()`（`main/app_ui.c:928`）
- 帧格式由底层初始化决定：`RGB565 + QVGA + fb_count=2 + PSRAM`（`main/esp32_s3_szp.c:546-550`）

3. 帧挂接到 UI 对象
- `img_camera_dsc.data = frame->buf`（`main/app_ui.c:929`）
- `lv_img_set_src(img_camera, &img_camera_dsc)`（`main/app_ui.c:930`）

4. 由 LVGL flush 到 LCD
- 显示驱动在 `bsp_display_lcd_init` 中注册（`main/esp32_s3_szp.c:375-404`）
- 启用了 `double_buffer=true` 与 `buff_dma=true`（`main/esp32_s3_szp.c:388-400`），最终通过 LCD 面板驱动发送到屏幕。

5. 归还摄像头帧缓冲
- `esp_camera_fb_return(frame)`（`main/app_ui.c:931`）

### 3.2 预留但未接入的路径（纯视频 pipeline）

`camera task -> xQueueLCDFrame -> lcd task -> esp_lcd_panel_draw_bitmap`
- 采集：`main/esp32_s3_szp.c:589`
- 入队：`main/esp32_s3_szp.c:591`
- 出队显示：`main/esp32_s3_szp.c:576-578`
- 归还帧：`main/esp32_s3_szp.c:579`

这条路径的数据边界更清晰（采集与显示彻底解耦），适合后续做帧丢弃策略和统计。

## 4. 卡顿 / 丢帧 / 同步问题与解决方式

## 4.1 已落地的解决策略（代码中已体现）

1. 卡顿：任务分核
- 多个关键任务都 `xTaskCreatePinnedToCore` 指定核（如 `main/main.c:66-67`、`main/app_ui.c:987`、`1640`），降低同核抢占导致的抖动。

2. 丢帧：摄像头双缓冲 + 抓帧策略
- `fb_count=2`（`main/esp32_s3_szp.c:549`）
- `CAMERA_GRAB_WHEN_EMPTY`（`main/esp32_s3_szp.c:551`）
- 作用：消费者慢时形成背压，优先保证系统稳定而不是无界抢帧。

3. 显示吞吐：LVGL 双缓冲 + DMA
- `double_buffer=true`、`buff_dma=true`（`main/esp32_s3_szp.c:388-400`）
- 作用：降低 flush 阻塞时间，减轻 UI 卡顿。

4. 同步：事件位 + UI 互斥锁
- EventGroup 保证任务阶段顺序（启动、WiFi、SNTP）
- `lvgl_port_lock/unlock` 保护跨任务 UI 修改。

## 4.2 当前仍存在的风险点（建议在下一版修复）

1. 摄像头任务里 `lv_img_set_src` 未加 `lvgl_port_lock`
- 位置：`main/app_ui.c:930`
- 风险：与其它 UI 线程并发访问 LVGL，可能出现偶发显示异常。

2. 未检查 `esp_camera_fb_get()` 为空
- 位置：`main/app_ui.c:928-931`
- 风险：低内存或瞬时异常时可能空指针解引用。

3. 帧指针生命周期偏紧
- 当前是“设 src -> 立即 return frame”（`main/app_ui.c:929-931`）
- 在高负载下可能出现撕裂/错帧窗口，建议改为“复制到专用显示缓冲”或改走 `xQueueLCDFrame` 双任务路径，确保消费完成后再归还。

## 4.3 建议的稳定性增强（可直接实施）

1. 给 `task_process_camera` 增加 `lvgl_port_lock/unlock` + `NULL` 判断。
2. 引入 FPS / 队列深度 / 丢帧计数日志，做可观测性闭环。
3. 把摄像头显示统一收敛到“采集-队列-显示”双任务模型，明确背压和丢帧策略。

## 5. 一句话总结

该项目采用的是“事件驱动入口 + 多任务并行执行 + EventGroup/Queue/共享状态/锁混合通信”的流水线架构；摄像头到显示链路已具备实时运行能力，并且已经使用双缓冲/DMA/任务分核来压制卡顿，但摄像头 UI 线程安全与帧生命周期仍有优化空间。
