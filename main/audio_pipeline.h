#pragma once

#include <stdio.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 音频播放三段流水线:
 *   文件读取任务 -> 环形缓冲区(rb_raw) -> MP3解码任务 -> 环形缓冲区(rb_pcm) -> I2S输出任务 -> ES8311
 * 控制命令(播放/暂停/恢复/停止)与事件(播放中/暂停/空闲)均通过FreeRTOS消息队列传递,
 * 环形缓冲区把读取、解码、输出三段解耦, 保证音频数据连续、播放不卡顿。
 */

/* 播放器状态 */
typedef enum {
    AUDIO_PIPELINE_STATE_IDLE = 0,   /* 空闲 */
    AUDIO_PIPELINE_STATE_PLAYING,    /* 播放中 */
    AUDIO_PIPELINE_STATE_PAUSED,     /* 暂停 */
} audio_pipeline_state_t;

/* 播放器事件(内部经消息队列汇总到控制任务后通知UI) */
typedef enum {
    AUDIO_PIPELINE_EVT_PLAYING = 0,  /* 开始播放 */
    AUDIO_PIPELINE_EVT_PAUSED,       /* 暂停播放 */
    AUDIO_PIPELINE_EVT_IDLE,         /* 播放结束或停止 */
} audio_pipeline_event_t;

/* 事件回调: 在控制任务上下文中调用 */
typedef void (*audio_pipeline_cb_t)(audio_pipeline_event_t event, void *arg);

/* 初始化流水线(创建环形缓冲区/消息队列/任务), 重复调用返回ESP_OK */
esp_err_t audio_pipeline_init(void);

/* 销毁流水线(停止播放并删除任务/缓冲区) */
void audio_pipeline_deinit(void);

/* 注册事件回调 */
void audio_pipeline_register_callback(audio_pipeline_cb_t cb, void *arg);

/* 播放一个已打开的音频文件, 文件指针交给流水线管理, 播放结束/停止后自动fclose */
esp_err_t audio_pipeline_play(FILE *fp);

/* 停止播放(未在播放时直接返回) */
esp_err_t audio_pipeline_stop(void);

/* 暂停播放 */
esp_err_t audio_pipeline_pause(void);

/* 恢复播放 */
esp_err_t audio_pipeline_resume(void);

/* 获取当前状态 */
audio_pipeline_state_t audio_pipeline_get_state(void);

#ifdef __cplusplus
}
#endif
