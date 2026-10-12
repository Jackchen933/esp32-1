#include <string.h>
#include <assert.h>
#include "audio_pipeline.h"
#include "esp32_s3_szp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/ringbuf.h"
#include "mp3dec.h"

static const char *TAG = "audio_pipeline";

/***************************  配置  ***************************/
#define RB_RAW_SIZE      (48 * 1024)  /* 环形缓冲区1: 文件读取 -> 解码 (原始MP3流) */
#define RB_PCM_SIZE      (48 * 1024)  /* 环形缓冲区2: 解码 -> I2S输出 (PCM) */
#define READ_CHUNK       (2048)       /* 文件读取任务单次读取字节数 */
#define DATA_BUF_SIZE    (4096)              /* 解码输入缓冲(读块2K+残余1.9K不会溢出) */
#define PCM_BUF_SAMPLES  (1152 * MAX_NCHAN)  /* 单帧最大PCM样本数 */

/* 事件组标志位 */
#define EG_ACTIVE        BIT0  /* 存在进行中的播放会话 */
#define EG_UNPAUSED      BIT1  /* 未暂停(播放中) */
#define EG_READ_DONE     BIT2  /* 文件读取完成 */
#define EG_DECODE_DONE   BIT3  /* 解码完成 */
#define EG_READER_IDLE   BIT4  /* 文件读取任务停靠 */
#define EG_DECODE_IDLE   BIT5  /* 解码任务停靠 */
#define EG_OUTPUT_IDLE   BIT6  /* I2S输出任务停靠 */

/* 控制命令消息 */
typedef enum {
    CMD_PLAY = 0,     /* 播放 */
    CMD_PAUSE,        /* 暂停 */
    CMD_RESUME,       /* 恢复 */
    CMD_STOP,         /* 停止 */
    CMD_SONG_END,     /* 输出任务报告: 整首歌已播放完毕 */
} pipeline_cmd_type_t;

typedef struct {
    pipeline_cmd_type_t type;
    FILE *fp;         /* CMD_PLAY: 待播放文件 */
} pipeline_cmd_t;

/****************************  状态  ****************************/
static RingbufHandle_t rb_raw = NULL;      /* 环形缓冲区: 原始MP3数据 */
static RingbufHandle_t rb_pcm = NULL;      /* 环形缓冲区: 解码后PCM */
static QueueHandle_t cmd_queue = NULL;     /* 命令消息队列 */
static EventGroupHandle_t pipe_eg = NULL;  /* 流水线状态事件组 */

static TaskHandle_t ctrl_task_handle = NULL;
static TaskHandle_t reader_task_handle = NULL;
static TaskHandle_t decode_task_handle = NULL;
static TaskHandle_t output_task_handle = NULL;

static FILE *cur_fp = NULL;                              /* 当前播放文件(控制任务独占) */
static volatile audio_pipeline_state_t pipe_state = AUDIO_PIPELINE_STATE_IDLE;
static volatile bool suppress_idle = false;              /* 切歌时抑制IDLE事件 防止误触发自动连播 */
static audio_pipeline_cb_t user_cb = NULL;
static void *user_cb_arg = NULL;
static bool inited = false;

/* 当前编解码器打开的采样格式(与bsp_codec_init默认值一致) */
static uint32_t open_rate = CODEC_DEFAULT_SAMPLE_RATE;
static uint32_t open_bits = CODEC_DEFAULT_BIT_WIDTH;

/***************************  内部函数  ***************************/

// 事件通知UI(控制任务上下文)
static void notify_user(audio_pipeline_event_t evt)
{
    if (user_cb && !suppress_idle) {
        user_cb(evt, user_cb_arg);
    }
}

// 清空环形缓冲区
static void drain_ring(RingbufHandle_t rb)
{
    size_t item_len;
    void *item;
    while ((item = xRingbufferReceive(rb, &item_len, 0)) != NULL) {
        vRingbufferReturnItem(rb, item);
    }
}

// 结束当前播放会话: 让三段任务停靠 -> 清空环形缓冲区 -> 关闭文件
static void stop_session(void)
{
    if (pipe_state == AUDIO_PIPELINE_STATE_IDLE) {
        return;
    }
    // 通知三段任务退出会话
    xEventGroupClearBits(pipe_eg, EG_ACTIVE | EG_UNPAUSED);
    // 等待三段任务全部停靠(最长2秒)
    xEventGroupWaitBits(pipe_eg, EG_READER_IDLE | EG_DECODE_IDLE | EG_OUTPUT_IDLE,
                        pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
    // 清空两个环形缓冲区中的残留数据
    drain_ring(rb_raw);
    drain_ring(rb_pcm);
    if (cur_fp) {
        fclose(cur_fp);
        cur_fp = NULL;
    }
    pipe_state = AUDIO_PIPELINE_STATE_IDLE;
    notify_user(AUDIO_PIPELINE_EVT_IDLE);
}

// 控制任务: 处理命令消息队列, 维护播放状态
static void control_task(void *arg)
{
    pipeline_cmd_t cmd;
    for (;;) {
        if (xQueueReceive(cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (cmd.type) {
        case CMD_PLAY:  // 开始播放新文件(先结束旧会话)
            suppress_idle = true;   // 切歌产生的IDLE事件不通知UI
            stop_session();
            suppress_idle = false;
            cur_fp = cmd.fp;
            xEventGroupClearBits(pipe_eg, EG_READ_DONE | EG_DECODE_DONE);
            pipe_state = AUDIO_PIPELINE_STATE_PLAYING;
            xEventGroupSetBits(pipe_eg, EG_ACTIVE | EG_UNPAUSED);
            ESP_LOGI(TAG, "start playing");
            notify_user(AUDIO_PIPELINE_EVT_PLAYING);
            break;
        case CMD_PAUSE:  // 暂停
            if (pipe_state == AUDIO_PIPELINE_STATE_PLAYING) {
                xEventGroupClearBits(pipe_eg, EG_UNPAUSED);
                pipe_state = AUDIO_PIPELINE_STATE_PAUSED;
                ESP_LOGI(TAG, "paused");
                notify_user(AUDIO_PIPELINE_EVT_PAUSED);
            }
            break;
        case CMD_RESUME:  // 恢复
            if (pipe_state == AUDIO_PIPELINE_STATE_PAUSED) {
                xEventGroupSetBits(pipe_eg, EG_UNPAUSED);
                pipe_state = AUDIO_PIPELINE_STATE_PLAYING;
                ESP_LOGI(TAG, "resumed");
                notify_user(AUDIO_PIPELINE_EVT_PLAYING);
            }
            break;
        case CMD_STOP:  // 停止
            stop_session();
            break;
        case CMD_SONG_END:  // 整首歌自然播放完毕
            if (pipe_state == AUDIO_PIPELINE_STATE_PLAYING) {
                ESP_LOGI(TAG, "song finished");
                stop_session();
            }
            break;
        default:
            break;
        }
    }
}

// 向环形缓冲区发送数据: 满则等待下游消费(背压), 会话结束则丢弃
static bool send_ring(RingbufHandle_t rb, const void *data, size_t len)
{
    while (xEventGroupGetBits(pipe_eg) & EG_ACTIVE) {
        if (xRingbufferSend(rb, data, len, pdMS_TO_TICKS(200)) == pdTRUE) {
            return true;
        }
    }
    return false;
}

// 第一段: 文件读取任务 MP3文件 -> 环形缓冲区rb_raw
static void reader_task(void *arg)
{
    static uint8_t read_buf[READ_CHUNK];
    for (;;) {
        EventBits_t bits = xEventGroupGetBits(pipe_eg);
        if (!(bits & EG_ACTIVE)) {  // 无会话 停靠
            xEventGroupSetBits(pipe_eg, EG_READER_IDLE);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        xEventGroupClearBits(pipe_eg, EG_READER_IDLE);

        if (!(xEventGroupGetBits(pipe_eg) & EG_UNPAUSED) ||   // 暂停中
            (xEventGroupGetBits(pipe_eg) & EG_READ_DONE)) {   // 已读完
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // 读取文件 -> 送入环形缓冲区
        size_t n = fread(read_buf, 1, READ_CHUNK, cur_fp);
        if (n > 0) {
            send_ring(rb_raw, read_buf, n);
        }
        if (n < READ_CHUNK) {  // 文件读完
            xEventGroupSetBits(pipe_eg, EG_READ_DONE);
            ESP_LOGI(TAG, "file read done");
        }
    }
}

// 第二段: MP3解码任务 环形缓冲区rb_raw -> Helix软解 -> 环形缓冲区rb_pcm
static void decode_task(void *arg)
{
    static uint8_t data_buf[DATA_BUF_SIZE];       // 解码输入缓冲
    static int16_t pcm_buf[PCM_BUF_SAMPLES];      // 解码输出PCM
    HMP3Decoder decoder = MP3InitDecoder();
    assert(decoder);

    for (;;) {
        EventBits_t bits = xEventGroupGetBits(pipe_eg);
        if (!(bits & EG_ACTIVE)) {  // 无会话 停靠
            xEventGroupSetBits(pipe_eg, EG_DECODE_IDLE);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        xEventGroupClearBits(pipe_eg, EG_DECODE_IDLE);

        /* 本会话的解码状态 */
        bool no_more_input = false;
        uint8_t *read_ptr = data_buf;
        size_t bytes_in_buf = 0;

        while (xEventGroupGetBits(pipe_eg) & EG_ACTIVE) {
            if (!(xEventGroupGetBits(pipe_eg) & EG_UNPAUSED)) {  // 暂停中
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            size_t unread = bytes_in_buf - (read_ptr - data_buf);

            /* 1. 从环形缓冲区rb_raw取原始MP3数据 补充到输入缓冲 */
            if (unread < MAINBUF_SIZE && !no_more_input) {
                if (read_ptr != data_buf) {  // 残余数据挪到头部
                    memmove(data_buf, read_ptr, unread);
                    read_ptr = data_buf;
                }
                size_t item_len;
                void *item = xRingbufferReceive(rb_raw, &item_len, pdMS_TO_TICKS(100));
                if (item) {
                    if (unread + item_len <= DATA_BUF_SIZE) {
                        memcpy(data_buf + unread, item, item_len);
                        bytes_in_buf = unread + item_len;
                    }
                    vRingbufferReturnItem(rb_raw, item);
                    continue;  // 尽量填满输入缓冲再解码
                }
                // 环形缓冲区已空
                if (xEventGroupGetBits(pipe_eg) & EG_READ_DONE) {
                    no_more_input = true;
                }
                if (unread == 0 && no_more_input) {
                    break;  // 文件已全部解码
                }
                continue;
            }

            /* 2. 查找MP3同步字 */
            int offset = MP3FindSyncWord(read_ptr, unread);
            if (offset < 0) {  // 未找到同步字 丢弃这段数据
                read_ptr += unread;
                continue;
            }
            read_ptr += offset;
            unread -= offset;

            /* 3. 解码一帧 */
            unsigned char *in_ptr = read_ptr;
            int in_left = (int)unread;
            int err = MP3Decode(decoder, &in_ptr, &in_left, pcm_buf, 0);
            read_ptr = in_ptr;
            if (err != ERR_MP3_NONE) {
                if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
                    // 数据不足 需要继续取数据
                    if (no_more_input &&
                        (bytes_in_buf - (read_ptr - data_buf)) < MAINBUF_SIZE) {
                        break;  // 输入耗尽仍未凑齐一帧 结束
                    }
                    continue;
                }
                // 坏帧: MP3Decode已消耗数据, 跳过继续
                continue;
            }

            /* 4. 根据帧信息设置编解码器采样率(变化时才重新配置) */
            MP3FrameInfo fi;
            MP3GetLastFrameInfo(decoder, &fi);
            if ((uint32_t)fi.samprate != open_rate || (uint32_t)fi.bitsPerSample != open_bits) {
                open_rate = fi.samprate;
                open_bits = fi.bitsPerSample;
                bsp_codec_set_fs(open_rate, open_bits, I2S_SLOT_MODE_STEREO);
                ESP_LOGI(TAG, "codec fs: %luHz %lubit", (unsigned long)open_rate, (unsigned long)open_bits);
            }

            /* 5. PCM送入环形缓冲区rb_pcm 供I2S输出任务消费 */
            size_t pcm_bytes = (size_t)fi.outputSamps * sizeof(int16_t);
            send_ring(rb_pcm, pcm_buf, pcm_bytes);
        }

        // 本会话解码结束 置标志供输出任务判断
        xEventGroupSetBits(pipe_eg, EG_DECODE_DONE);
        while (xEventGroupGetBits(pipe_eg) & EG_ACTIVE) {  // 等待控制任务结束会话
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        xEventGroupClearBits(pipe_eg, EG_DECODE_DONE);
    }
}

// 第三段: I2S输出任务 环形缓冲区rb_pcm -> I2S DMA -> ES8311
static void output_task(void *arg)
{
    for (;;) {
        EventBits_t bits = xEventGroupGetBits(pipe_eg);
        if (!(bits & EG_ACTIVE)) {  // 无会话 停靠
            xEventGroupSetBits(pipe_eg, EG_OUTPUT_IDLE);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        xEventGroupClearBits(pipe_eg, EG_OUTPUT_IDLE);

        size_t item_len;
        void *item = xRingbufferReceive(rb_pcm, &item_len, pdMS_TO_TICKS(100));
        if (item) {
            // 暂停时等待恢复(该块PCM已出队, 恢复后继续输出)
            while ((xEventGroupGetBits(pipe_eg) & EG_ACTIVE) &&
                   !(xEventGroupGetBits(pipe_eg) & EG_UNPAUSED)) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (xEventGroupGetBits(pipe_eg) & EG_ACTIVE) {
                size_t written;
                bsp_i2s_write(item, item_len, &written, 1000);
            }
            vRingbufferReturnItem(rb_pcm, item);
            continue;
        }

        // 环形缓冲区已空: 检查是否整首歌解码输出完毕
        if (xEventGroupGetBits(pipe_eg) & EG_DECODE_DONE) {
            pipeline_cmd_t cmd = {.type = CMD_SONG_END, .fp = NULL};
            xQueueSend(cmd_queue, &cmd, 0);
            // 等待会话结束或恢复播放(暂停时播完 等待恢复后重新上报SONG_END)
            while ((xEventGroupGetBits(pipe_eg) & EG_ACTIVE) &&
                   !(xEventGroupGetBits(pipe_eg) & EG_UNPAUSED)) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (xEventGroupGetBits(pipe_eg) & EG_ACTIVE) {
                vTaskDelay(pdMS_TO_TICKS(100));  // 等待控制任务处理SONG_END
            }
        }
    }
}

/***************************  对外接口  ***************************/

esp_err_t audio_pipeline_init(void)
{
    if (inited) {
        return ESP_OK;
    }
    // 两个环形缓冲区放入PSRAM 减少内部RAM占用
    rb_raw = xRingbufferCreateWithCaps(RB_RAW_SIZE, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    rb_pcm = xRingbufferCreateWithCaps(RB_PCM_SIZE, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(rb_raw && rb_pcm);

    cmd_queue = xQueueCreate(8, sizeof(pipeline_cmd_t));
    assert(cmd_queue);
    pipe_eg = xEventGroupCreate();
    assert(pipe_eg);

    // 控制任务: 命令消息队列处理
    xTaskCreatePinnedToCore(control_task, "audio_ctrl", 3 * 1024, NULL, 5, &ctrl_task_handle, 0);
    // 文件读取任务
    xTaskCreatePinnedToCore(reader_task, "audio_read", 3 * 1024, NULL, 5, &reader_task_handle, 0);
    // MP3解码任务
    xTaskCreatePinnedToCore(decode_task, "audio_dec", 6 * 1024, NULL, 5, &decode_task_handle, 1);
    // I2S输出任务
    xTaskCreatePinnedToCore(output_task, "audio_out", 3 * 1024, NULL, 6, &output_task_handle, 1);

    pipe_state = AUDIO_PIPELINE_STATE_IDLE;
    inited = true;
    ESP_LOGI(TAG, "audio pipeline ready (reader->rb_raw->decoder->rb_pcm->i2s out)");
    return ESP_OK;
}

void audio_pipeline_deinit(void)
{
    if (!inited) {
        return;
    }
    // 先停止播放
    pipeline_cmd_t cmd = {.type = CMD_STOP, .fp = NULL};
    xQueueSend(cmd_queue, &cmd, pdMS_TO_TICKS(200));
    int wait = 0;
    while (pipe_state != AUDIO_PIPELINE_STATE_IDLE && wait++ < 20) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    // 删除任务
    vTaskDelete(ctrl_task_handle);
    vTaskDelete(reader_task_handle);
    vTaskDelete(decode_task_handle);
    vTaskDelete(output_task_handle);

    // 删除队列/事件组/环形缓冲区
    vQueueDelete(cmd_queue);
    vEventGroupDelete(pipe_eg);
    vRingbufferDelete(rb_raw);
    vRingbufferDelete(rb_pcm);
    cmd_queue = NULL;
    pipe_eg = NULL;
    rb_raw = NULL;
    rb_pcm = NULL;

    inited = false;
    pipe_state = AUDIO_PIPELINE_STATE_IDLE;
    ESP_LOGI(TAG, "audio pipeline deleted");
}

void audio_pipeline_register_callback(audio_pipeline_cb_t cb, void *arg)
{
    user_cb = cb;
    user_cb_arg = arg;
}

esp_err_t audio_pipeline_play(FILE *fp)
{
    if (!inited || fp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    pipeline_cmd_t cmd = {.type = CMD_PLAY, .fp = fp};
    if (xQueueSend(cmd_queue, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) {
        fclose(fp);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t audio_pipeline_stop(void)
{
    if (!inited) {
        return ESP_ERR_INVALID_STATE;
    }
    pipeline_cmd_t cmd = {.type = CMD_STOP, .fp = NULL};
    return (xQueueSend(cmd_queue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_pipeline_pause(void)
{
    if (!inited) {
        return ESP_ERR_INVALID_STATE;
    }
    pipeline_cmd_t cmd = {.type = CMD_PAUSE, .fp = NULL};
    return (xQueueSend(cmd_queue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t audio_pipeline_resume(void)
{
    if (!inited) {
        return ESP_ERR_INVALID_STATE;
    }
    pipeline_cmd_t cmd = {.type = CMD_RESUME, .fp = NULL};
    return (xQueueSend(cmd_queue, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

audio_pipeline_state_t audio_pipeline_get_state(void)
{
    return pipe_state;
}
