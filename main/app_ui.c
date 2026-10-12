#include "app_ui.h"
#include "audio_pipeline.h"
#include "esp32_s3_szp.h"
#include "file_iterator.h"
#include "string.h"
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "app_ui";

LV_FONT_DECLARE(font_alipuhui20);

lv_obj_t * main_obj; // 主界面
lv_obj_t * main_text_label; // 主界面 欢迎语
lv_obj_t * icon_in_obj; // 应用界面
int icon_flag; // 标记现在进入哪个应用 在主界面时为0

/*********************** 开机界面 ****************************/
lv_obj_t * lckfb_logo;
// 设置角度的回调函数
static void set_angle(void * img, int32_t v)
{
    lv_img_set_angle(img, v); // 设置图片的旋转角度
}
// 开机界面
void lv_gui_start(void)
{
    lvgl_port_lock(0);
    // 显示logo
    LV_IMG_DECLARE(image_lckfb_logo);  // 声明图片
    lckfb_logo = lv_img_create(lv_scr_act()); // 创建图片对象
    lv_img_set_src(lckfb_logo, &image_lckfb_logo); // 设置图片对象的图片源
    lv_obj_align(lckfb_logo, LV_ALIGN_CENTER, 0, 0); // 设置图片位置为屏幕正中心
    lv_img_set_pivot(lckfb_logo, 60, 60); // 设置图片围绕自己的中心位置旋转

    // 设置旋转动画
    lv_anim_t a; // 创建动画变量
    lv_anim_init(&a); // 初始化动画变量
    lv_anim_set_var(&a, lckfb_logo); // 动画变量赋值为logo图片
    lv_anim_set_exec_cb(&a, set_angle); // 创建设置角度的回调函数
    lv_anim_set_values(&a, 0, 3600); // 设置动画旋转角度的开始值和结尾值
    lv_anim_set_time(&a, 200); // 设置转一圈的周期是200毫秒
    lv_anim_set_repeat_count(&a, 5); // 设置旋转5次
    lv_anim_start(&a); // 动画开始
    lvgl_port_unlock();
}


/*********************  第1个图标  音乐播放器 *********************************************************************************************/
static uint8_t g_sys_volume = VOLUME_DEFAULT;
static file_iterator_instance_t *file_iterator = NULL;

lv_obj_t *music_list;
lv_obj_t *label_play_pause;
lv_obj_t *btn_play_pause;
lv_obj_t *volume_slider;

lv_obj_t *music_title_label;
lv_obj_t *btn_music_back;

// 播放指定序号的音乐
static void play_index(int index)
{
    ESP_LOGI(TAG, "play_index(%d)", index);

    char filename[128];
    int retval = file_iterator_get_full_path_from_index(file_iterator, index, filename, sizeof(filename));
    if (retval == 0) {
        ESP_LOGE(TAG, "unable to retrieve filename");
        return;
    }

    FILE *fp = fopen(filename, "rb");
    if (fp) {
        ESP_LOGI(TAG, "Playing '%s'", filename);
        // 文件交给音频流水线: 读取->解码->I2S输出 三段流水线播放
        audio_pipeline_play(fp);
    } else {
        ESP_LOGE(TAG, "unable to open index %d, filename '%s'", index, filename);
    }
}

// 播放器事件回调 控制任务通过消息队列通知
static void audio_pipeline_evt_cb(audio_pipeline_event_t event, void *arg)
{
    switch (event) {
    case AUDIO_PIPELINE_EVT_IDLE: {  // 播放结束/停止
        pa_en(0); // 关闭音频功放
        // 播放完一首歌 自动指向下一首(音乐列表可用时)
        if (file_iterator == NULL) {
            break;
        }
        file_iterator_next(file_iterator);
        int index = file_iterator_get_index(file_iterator);
        ESP_LOGI(TAG, "playing index '%d'", index);
        play_index(index);
        // 修改当前播放的音乐名称(音乐页面打开时)
        if (icon_flag == 1) {
            lvgl_port_lock(0);
            lv_dropdown_set_selected(music_list, index);
            lvgl_port_unlock();
        }
        break;
    }
    case AUDIO_PIPELINE_EVT_PLAYING: // 正在播放音乐
        pa_en(1); // 打开音频功放
        break;
    case AUDIO_PIPELINE_EVT_PAUSED:  // 正在暂停音乐
        pa_en(0); // 关闭音频功放
        break;
    default:
        break;
    }
}

// mp3播放器初始化
void mp3_player_init(void)
{
    // 获取SPI Flash FATFS中的文件信息
    file_iterator = file_iterator_new(FLASH_FATFS_BASE);
    assert(file_iterator != NULL);

    // 初始化音频播放流水线
    ESP_ERROR_CHECK(audio_pipeline_init());
    audio_pipeline_register_callback(audio_pipeline_evt_cb, NULL);
}


// 按钮样式相关定义
typedef struct {
    lv_style_t style_bg;
    lv_style_t style_focus_no_outline;
} button_style_t;

static button_style_t g_btn_styles;

button_style_t *ui_button_styles(void)
{
    return &g_btn_styles;
}

// 按钮样式初始化
static void ui_button_style_init(void)
{
    /*Init the style for the default state*/
    lv_style_init(&g_btn_styles.style_focus_no_outline);
    lv_style_set_outline_width(&g_btn_styles.style_focus_no_outline, 0);

    lv_style_init(&g_btn_styles.style_bg);
    lv_style_set_bg_opa(&g_btn_styles.style_bg, LV_OPA_100);
    lv_style_set_bg_color(&g_btn_styles.style_bg, lv_color_make(255, 255, 255));
    lv_style_set_shadow_width(&g_btn_styles.style_bg, 0);
}

// 播放暂停按钮 事件处理函数
static void btn_play_pause_cb(lv_event_t *event)
{
    lv_obj_t *btn = lv_event_get_target(event);
    lv_obj_t *lab = (lv_obj_t *) btn->user_data;

    audio_pipeline_state_t state = audio_pipeline_get_state();
    printf("state=%d\n", state);
    if(state == AUDIO_PIPELINE_STATE_IDLE){
        lvgl_port_lock(0);
        lv_label_set_text_static(lab, LV_SYMBOL_PAUSE);
        lvgl_port_unlock();
        int index = file_iterator_get_index(file_iterator);
        ESP_LOGI(TAG, "playing index '%d'", index);
        play_index(index);
    }else if (state == AUDIO_PIPELINE_STATE_PAUSED) {
        lvgl_port_lock(0);
        lv_label_set_text_static(lab, LV_SYMBOL_PAUSE);
        lvgl_port_unlock();
        audio_pipeline_resume();
    } else if (state == AUDIO_PIPELINE_STATE_PLAYING) {
        lvgl_port_lock(0);
        lv_label_set_text_static(lab, LV_SYMBOL_PLAY);
        lvgl_port_unlock();
        audio_pipeline_pause();
    }
}

// 上一首 下一首 按键事件处理函数
static void btn_prev_next_cb(lv_event_t *event)
{
    bool is_next = (bool) event->user_data;

    if (is_next) {
        ESP_LOGI(TAG, "btn next");
        file_iterator_next(file_iterator);
    } else {
        ESP_LOGI(TAG, "btn prev");
        file_iterator_prev(file_iterator);
    }
    // 修改当前的音乐名称
    int index = file_iterator_get_index(file_iterator);
    lvgl_port_lock(0);
    lv_dropdown_set_selected(music_list, index);
    // lv_obj_t *label_title = (lv_obj_t *) music_list->user_data;
    // lv_label_set_text_static(label_title, file_iterator_get_name_from_index(file_iterator, index));
    lvgl_port_unlock();
    // 执行音乐事件
    audio_pipeline_state_t state = audio_pipeline_get_state();
    printf("prev_next_state=%d\n", state);
    if (state == AUDIO_PIPELINE_STATE_IDLE) {
        // Nothing to do
    }else if (state == AUDIO_PIPELINE_STATE_PAUSED){ // 如果当前正在暂停歌曲
        ESP_LOGI(TAG, "playing index '%d'", index);
        play_index(index);
        audio_pipeline_pause();
    } else if (state == AUDIO_PIPELINE_STATE_PLAYING) { // 如果当前正在播放歌曲
        // 播放歌曲
        ESP_LOGI(TAG, "playing index '%d'", index);
        play_index(index);
    }
}

// 音量调节滑动条 事件处理函数
static void volume_slider_cb(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    int volume = lv_slider_get_value(slider); // 获取slider的值
    bsp_codec_volume_set(volume, NULL); // 设置声音大小
    g_sys_volume = volume; // 把声音赋值给g_sys_volume保存
    ESP_LOGI(TAG, "volume '%d'", volume);
}

// 音乐列表 点击事件处理函数
static void music_list_cb(lv_event_t *event)
{
    uint16_t index = lv_dropdown_get_selected(music_list);
    ESP_LOGI(TAG, "switching index to '%d'", index);
    file_iterator_set_index(file_iterator, index);

    audio_pipeline_state_t state = audio_pipeline_get_state();
    if (state == AUDIO_PIPELINE_STATE_PAUSED){ // 如果当前正在暂停歌曲
        play_index(index);
        audio_pipeline_pause();
    } else if (state == AUDIO_PIPELINE_STATE_PLAYING) { // 如果当前正在播放歌曲
        play_index(index);
    }
}

// 音乐名称加入列表
static void build_file_list(lv_obj_t *music_list)
{
    lvgl_port_lock(0);
    lv_dropdown_clear_options(music_list);
    lvgl_port_unlock();

    for(size_t i = 0; i<file_iterator->count; i++)
    {
        const char *file_name = file_iterator_get_name_from_index(file_iterator, i);
        if (NULL != file_name) {
            lvgl_port_lock(0);
            lv_dropdown_add_option(music_list, file_name, i); // 添加音乐名称到列表中
            lvgl_port_unlock();
        }
    }
    lvgl_port_lock(0);
    lv_dropdown_set_selected(music_list, 0); // 选择列表中的第一个
    lvgl_port_unlock();
}

// 播放器界面初始化
void music_ui(void)
{
    lvgl_port_lock(0);

    ui_button_style_init();// 初始化按键风格

    /* 创建播放暂停控制按键 */
    btn_play_pause = lv_btn_create(icon_in_obj);
    lv_obj_align(btn_play_pause, LV_ALIGN_CENTER, 0, 40);
    lv_obj_set_size(btn_play_pause, 50, 50);
    lv_obj_set_style_radius(btn_play_pause, 25, LV_STATE_DEFAULT);
    lv_obj_add_flag(btn_play_pause, LV_OBJ_FLAG_CHECKABLE);

    lv_obj_add_style(btn_play_pause, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_pause, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUSED);

    label_play_pause = lv_label_create(btn_play_pause);

    lv_label_set_text_static(label_play_pause, LV_SYMBOL_PLAY);
    lv_obj_center(label_play_pause);

    lv_obj_set_user_data(btn_play_pause, (void *) label_play_pause);
    lv_obj_add_event_cb(btn_play_pause, btn_play_pause_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* 创建上一首控制按键 */
    lv_obj_t *btn_play_prev = lv_btn_create(icon_in_obj);
    lv_obj_set_size(btn_play_prev, 50, 50);
    lv_obj_set_style_radius(btn_play_prev, 25, LV_STATE_DEFAULT);
    lv_obj_clear_flag(btn_play_prev, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_align_to(btn_play_prev, btn_play_pause, LV_ALIGN_OUT_LEFT_MID, -40, 0);

    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_bg, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_bg, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_play_prev, &ui_button_styles()->style_bg, LV_STATE_DEFAULT);

    lv_obj_t *label_prev = lv_label_create(btn_play_prev);
    lv_label_set_text_static(label_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_font(label_prev, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_prev, lv_color_make(0, 0, 0), LV_STATE_DEFAULT);
    lv_obj_center(label_prev);
    lv_obj_set_user_data(btn_play_prev, (void *) label_prev);
    lv_obj_add_event_cb(btn_play_prev, btn_prev_next_cb, LV_EVENT_CLICKED, (void *) false);

    /* 创建下一首控制按键 */
    lv_obj_t *btn_play_next = lv_btn_create(icon_in_obj);
    lv_obj_set_size(btn_play_next, 50, 50);
    lv_obj_set_style_radius(btn_play_next, 25, LV_STATE_DEFAULT);
    lv_obj_clear_flag(btn_play_next, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_align_to(btn_play_next, btn_play_pause, LV_ALIGN_OUT_RIGHT_MID, 40, 0);

    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_focus_no_outline, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_bg, LV_STATE_FOCUS_KEY);
    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_bg, LV_STATE_FOCUSED);
    lv_obj_add_style(btn_play_next, &ui_button_styles()->style_bg, LV_STATE_DEFAULT);

    lv_obj_t *label_next = lv_label_create(btn_play_next);
    lv_label_set_text_static(label_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_font(label_next, &lv_font_montserrat_24, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label_next, lv_color_make(0, 0, 0), LV_STATE_DEFAULT);
    lv_obj_center(label_next);
    lv_obj_set_user_data(btn_play_next, (void *) label_next);
    lv_obj_add_event_cb(btn_play_next, btn_prev_next_cb, LV_EVENT_CLICKED, (void *) true);

    /* 创建声音调节滑动条 */
    volume_slider = lv_slider_create(icon_in_obj);
    lv_obj_set_size(volume_slider, 200, 10);
    lv_obj_set_ext_click_area(volume_slider, 15);
    lv_obj_align(volume_slider, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, g_sys_volume, LV_ANIM_ON);
    lv_obj_add_event_cb(volume_slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *lab_vol_min = lv_label_create(icon_in_obj);
    lv_label_set_text_static(lab_vol_min, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_font(lab_vol_min, &lv_font_montserrat_20, LV_STATE_DEFAULT);
    lv_obj_align_to(lab_vol_min, volume_slider, LV_ALIGN_OUT_LEFT_MID, -10, 0);

    lv_obj_t *lab_vol_max = lv_label_create(icon_in_obj);
    lv_label_set_text_static(lab_vol_max, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(lab_vol_max, &lv_font_montserrat_20, LV_STATE_DEFAULT);
    lv_obj_align_to(lab_vol_max, volume_slider, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    /* 创建音乐列表 */
    music_list = lv_dropdown_create(icon_in_obj);
    lv_dropdown_clear_options(music_list);
    lv_dropdown_set_options_static(music_list, "扫描中...");
    lv_obj_set_style_text_font(music_list, &font_alipuhui20, LV_STATE_ANY);
    lv_obj_set_width(music_list, 200);
    lv_obj_align(music_list, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_event_cb(music_list, music_list_cb, LV_EVENT_VALUE_CHANGED, NULL);

    build_file_list(music_list);

    lvgl_port_unlock();
}

// 返回主界面按钮事件处理函数
static void btn_music_back_cb(lv_event_t * e)
{
    lv_obj_del(icon_in_obj);
    file_iterator = NULL;    // 先置空 防止停止播放时的IDLE事件误触发自动连播
    audio_pipeline_deinit();
    icon_flag = 0;
}

// 进入音乐播放应用
static void music_event_handler(lv_event_t * e)
{
    // 初始化mp3播放器
    mp3_player_init();
    // 创建一个界面对象
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 10);
    lv_style_set_bg_opa( &style, LV_OPA_COVER );
    lv_style_set_bg_color(&style, lv_color_hex(0xffffff));
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_width(&style, 320);
    lv_style_set_height(&style, 240);

    icon_in_obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(icon_in_obj, &style, 0);

    // 创建标题背景
    lv_obj_t *music_title = lv_obj_create(icon_in_obj);
    lv_obj_set_size(music_title, 320, 40);
    lv_obj_set_style_pad_all(music_title, 0, 0);  // 设置间隙
    lv_obj_align(music_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(music_title, lv_color_hex(0xf87c30), 0);
    // 显示标题
    music_title_label = lv_label_create(music_title);
    lv_label_set_text(music_title_label, "音乐播放器");
    lv_obj_set_style_text_color(music_title_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(music_title_label, &font_alipuhui20, 0);
    lv_obj_align(music_title_label, LV_ALIGN_CENTER, 0, 0);
    // 创建后退按钮
    btn_music_back = lv_btn_create(music_title);
    lv_obj_align(btn_music_back, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(btn_music_back, 60, 30);
    lv_obj_set_style_border_width(btn_music_back, 0, 0); // 设置边框宽度
    lv_obj_set_style_pad_all(btn_music_back, 0, 0);  // 设置间隙
    lv_obj_set_style_bg_opa(btn_music_back, LV_OPA_TRANSP, LV_PART_MAIN); // 背景透明
    lv_obj_set_style_shadow_opa(btn_music_back, LV_OPA_TRANSP, LV_PART_MAIN); // 阴影透明
    lv_obj_add_event_cb(btn_music_back, btn_music_back_cb, LV_EVENT_CLICKED, NULL); // 添加按键处理函数

    lv_obj_t *label_back = lv_label_create(btn_music_back);
    lv_label_set_text(label_back, LV_SYMBOL_LEFT);  // 按键上显示左箭头符号
    lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0);
    lv_obj_align(label_back, LV_ALIGN_CENTER, -10, 0);

    icon_flag = 1; // 标记已经进入第一个应用

    music_ui(); // 音乐播放器界面
}


/******************************** 第2个图标  SD卡 应用程序***************************************************************************/
lv_obj_t * sdcard_title; // SD卡页面标题背景
lv_obj_t * sdcard_label; // SD卡页面标题
lv_obj_t * sdcard_file_list; // SD卡文件列表

extern sdmmc_card_t *sdmmc_card;

struct file_path_info
{
    uint8_t path_index;  // 在第几级目录
    char path_now[512]; // 当前文件路径
    char path_back[512]; // 上级文件路径
};
struct file_path_info file_path_info;

// 函数声明
esp_err_t list_sdcard_files(char * path);
static void file_list_btn_cb(lv_event_t * e);

// 返回主界面按钮事件处理函数
static void btn_sdback_cb(lv_event_t * e)
{
    if (file_path_info.path_index == 0){ // 如果当前是根目录
        audio_pipeline_stop(); // 停止可能正在播放的SD卡音乐
        bsp_sdcard_unmount(); // 卸载SD卡
        lv_obj_del(icon_in_obj); // 回到主界面
    }else{
        lv_obj_clean(sdcard_file_list); // 清除当前文件列表
        esp_err_t ret = list_sdcard_files(file_path_info.path_back); // 列出上一级目录文件
        if (ret == ESP_OK){ // 如果成功列出目录
            strcpy(file_path_info.path_now, file_path_info.path_back); // 刚刚进入的这个目录路径 变成当前路径
            file_path_info.path_index--; // 目录级数索引退一级
            // 计算再向下退一级的目录路径
            char *slash = strrchr(file_path_info.path_back, '/'); // 从后往前查找字符'/'
            if (slash!= NULL) { // 如果查找到
                *slash = '\0'; // 替换为NULL 表示字符串结束
            }
            ESP_LOGI(TAG, "path_index: %d", file_path_info.path_index);
            ESP_LOGI(TAG, "path_now: %s", file_path_info.path_now);
            ESP_LOGI(TAG, "path_back: %s", file_path_info.path_back);
        }
    }
}

// 列出SD卡中的文件
esp_err_t list_sdcard_files(char * path)
{
    esp_err_t ret;
    DIR *dir;
    struct dirent *ent;
    lv_obj_t * btn;
    if ((dir = opendir(path))!= NULL) { // 打开目录
        while ((ent = readdir(dir))!= NULL) { // 读取目录中的文件
            /* 常规文件处理 */
            if (ent->d_type == DT_REG){ // 如果是常规文件
                int file_type_flag = 0;
                const char* extension = strrchr(ent->d_name, '.'); // 从后往前 找到字符'.'
                if (extension != NULL) { // 如果找到了'.'
                    extension++; // 指针地址+1
                    if (strcmp(extension, "mp3") == 0) { // 如果是mp3
                        file_type_flag = 1; // 标记为音乐文件
                    } else if (strcmp(extension, "wav") == 0) { // 如果是wav
                        file_type_flag = 1; // 标记为音乐文件
                    } else if (strcmp(extension, "mp4") == 0) {
                        file_type_flag = 2; // 标记为视频文件
                    } else if (strcmp(extension, "avi") == 0) {
                        file_type_flag = 2; // 标记为视频文件
                    } else if (strcmp(extension, "jpg") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else if (strcmp(extension, "jpeg") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else if (strcmp(extension, "png") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else if (strcmp(extension, "bmp") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else if (strcmp(extension, "gif") == 0) {
                        file_type_flag = 3; // 标记为图片
                    } else {
                        file_type_flag = 0; // 除了以上文件 其它文件都归类为普通文件
                    }
                }
                lvgl_port_lock(0);
                switch (file_type_flag)
                {
                case 1:
                    btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_AUDIO, (const char *)ent->d_name);  // 显示音乐文件图标
                    break;
                case 2:
                    btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_VIDEO, (const char *)ent->d_name);  // 显示视频文件图标
                    break;
                case 3:
                    btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_IMAGE, (const char *)ent->d_name);  // 显示图片文件图标
                    break;
                default:
                    btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_FILE, (const char *)ent->d_name);  // 显示普通文件图标
                    break;
                }
                lv_obj_t *icon = lv_obj_get_child(btn, 0); // 获取图标指针
                lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0); // 修改图标的字体
                lv_obj_add_event_cb(btn, file_list_btn_cb, LV_EVENT_CLICKED, NULL);  // 添加点击回调函数
                lvgl_port_unlock();
            }
            /* 文件夹处理 */
            else if (ent->d_type == DT_DIR) { // 如果是文件夹
                lvgl_port_lock(0);
                btn = lv_list_add_btn(sdcard_file_list, LV_SYMBOL_DIRECTORY, (const char *)ent->d_name);
                lv_obj_t *icon = lv_obj_get_child(btn, 0); // 获取图标指针
                lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0); // 修改图标的字体
                lv_obj_add_event_cb(btn, file_list_btn_cb, LV_EVENT_CLICKED, NULL); // 添加点击回调函数
                lvgl_port_unlock();
            }
        }
        closedir(dir);
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to open directory %s.", path);
        ret = ESP_FAIL;
    }
    return ret;
}

// 文件点击 事件处理函数
static void file_list_btn_cb(lv_event_t * e)
{
    const char *file_name = NULL; // 当前文件名称
    // 获取点击的按钮名称 即文件名称
    file_name = lv_list_get_btn_text(lv_obj_get_parent(e->target), e->target);
    ESP_LOGI(TAG, "file name: %s", file_name);
    // 列出 SD 卡中的文件
    struct stat st; // 获取文件状态信息结构体
    strcpy(file_path_info.path_back, file_path_info.path_now); // 保存上一级目录
    strcat(file_path_info.path_now, "/");
    strcat(file_path_info.path_now, file_name);
    if (stat(file_path_info.path_now, &st) == 0){ // 如果成功获取到状态信息
        if (S_ISDIR(st.st_mode)){ // 如果是目录
            lv_obj_clean(sdcard_file_list); // 清除当前文件列表
            esp_err_t ret = list_sdcard_files(file_path_info.path_now);
            if (ret == ESP_OK){ // 如果成功列出了目录
                file_path_info.path_index++; // 表示进入到下一集目录
                ESP_LOGI(TAG, "path_index: %d", file_path_info.path_index);
                ESP_LOGI(TAG, "path_now: %s", file_path_info.path_now);
                ESP_LOGI(TAG, "path_back: %s", file_path_info.path_back);
            }
            return;
        }
    }
    // 如果是mp3文件 动态加载到音频流水线播放
    const char *extension = strrchr(file_name, '.');
    if (extension != NULL && strcmp(extension + 1, "mp3") == 0) {
        audio_pipeline_init(); // 音乐页面未打开时在此初始化流水线(重复调用安全)
        audio_pipeline_register_callback(audio_pipeline_evt_cb, NULL);
        bsp_codec_volume_set(g_sys_volume, NULL); // 沿用当前设置的音量
        FILE *fp = fopen(file_path_info.path_now, "rb");
        if (fp) {
            ESP_LOGI(TAG, "Playing from SD: %s", file_path_info.path_now);
            audio_pipeline_play(fp); // SD卡音频资源动态加载
        } else {
            ESP_LOGE(TAG, "open failed: %s", file_path_info.path_now);
        }
    }
    // 如果没有成功进入目录
    strcpy(file_path_info.path_now, file_path_info.path_back); // 没有列出新的列表 还原当前路径
    // 还原再向下退一级的目录路径
    char *slash = strrchr(file_path_info.path_back, '/'); // 从后往前查找字符'/'
    if (slash!= NULL) { // 如果查找到
        *slash = '\0'; // 替换为NULL 表示字符串结束
    }
    ESP_LOGI(TAG, "path_index: %d", file_path_info.path_index);
    ESP_LOGI(TAG, "path_now: %s", file_path_info.path_now);
    ESP_LOGI(TAG, "path_back: %s", file_path_info.path_back);
}

// SD卡处理任务
static void task_process_sdcard(void *arg)
{
    esp_err_t ret;

    ret = bsp_sdcard_mount(); // 挂载SD卡
    if(ret != ESP_OK){ // 如果没有挂载成功
        ESP_LOGE(TAG, "Failed to mount filesystem.");
        lvgl_port_lock(0);
        lv_label_set_text(sdcard_label, "SD卡挂载不成功");
        lvgl_port_unlock();
        vTaskDelay(1000 / portTICK_PERIOD_MS); // 给上面一点显示的时间
        lvgl_port_lock(0);
        lv_obj_del(icon_in_obj);
        lvgl_port_unlock();
    }else{ // 如果挂载成功
        // 终端显示SD卡信息
        sdmmc_card_print_info(stdout, sdmmc_card);
        // 液晶屏标题栏显示SD卡容量
        lvgl_port_lock(0);
        lv_label_set_text_fmt(sdcard_label, "SD: %lluGB",
            (((uint64_t)sdmmc_card->csd.capacity) * sdmmc_card->csd.sector_size) >> 30);
        lvgl_port_unlock();

        // 创建返回按钮
        lvgl_port_lock(0);
        lv_obj_t *btn_back = lv_btn_create(sdcard_title);
        lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_size(btn_back, 60, 30);
        lv_obj_set_style_border_width(btn_back, 0, 0); // 设置边框宽度
        lv_obj_set_style_pad_all(btn_back, 0, 0);  // 设置间隙
        lv_obj_set_style_bg_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN); // 背景透明
        lv_obj_set_style_shadow_opa(btn_back, LV_OPA_TRANSP, LV_PART_MAIN); // 阴影透明
        lv_obj_add_event_cb(btn_back, btn_sdback_cb, LV_EVENT_CLICKED, NULL); // 添加按键处理函数

        lv_obj_t *label_back = lv_label_create(btn_back);
        lv_label_set_text(label_back, LV_SYMBOL_LEFT);  // 按键上显示左箭头符号
        lv_obj_set_style_text_font(label_back, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label_back, lv_color_hex(0xffffff), 0);
        lv_obj_align(label_back, LV_ALIGN_CENTER, -10, 0);

        // 创建文件列表
        sdcard_file_list = lv_list_create(icon_in_obj);
        lv_obj_set_size(sdcard_file_list, 320, 200);
        lv_obj_align(sdcard_file_list, LV_ALIGN_TOP_LEFT, 0, 40);
        lv_obj_set_style_border_width(sdcard_file_list, 0, 0);
        lv_obj_set_style_text_font(sdcard_file_list, &font_alipuhui20, 0);
        lv_obj_set_scrollbar_mode(sdcard_file_list, LV_SCROLLBAR_MODE_OFF); // 隐藏文件列表滚动条
        lvgl_port_unlock();
        // 列出 SD 卡中的文件
        file_path_info.path_index = 0; // 表示当前在根目录
        strcpy(file_path_info.path_now, SD_MOUNT_POINT); // 装入当前路径
        list_sdcard_files(file_path_info.path_now); // 列出当前目录文件
    }

    vTaskDelete(NULL);
}


// 进入SD卡应用程序
static void sdcard_event_handler(lv_event_t * e)
{
    // 创建一个界面对象
    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 10);
    lv_style_set_bg_opa( &style, LV_OPA_COVER );
    lv_style_set_bg_color(&style, lv_color_hex(0xffffff));
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_width(&style, 320);
    lv_style_set_height(&style, 240);

    icon_in_obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(icon_in_obj, &style, 0);

    // 创建标题背景
    sdcard_title = lv_obj_create(icon_in_obj);
    lv_obj_set_size(sdcard_title, 320, 40);
    lv_obj_set_style_pad_all(sdcard_title, 0, 0);  // 设置间隙
    lv_obj_align(sdcard_title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(sdcard_title, lv_color_hex(0x008b8b), 0);
    // 显示标题
    sdcard_label = lv_label_create(sdcard_title);
    lv_label_set_text(sdcard_label, "TF卡扫描中...");
    lv_obj_set_style_text_color(sdcard_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(sdcard_label, &font_alipuhui20, 0);
    lv_obj_align(sdcard_label, LV_ALIGN_CENTER, 0, 0);

    icon_flag = 2; // 标记已经进入第二个应用

    xTaskCreatePinnedToCore(task_process_sdcard, "task_process_sdcard", 3 * 1024, NULL, 5, NULL, 1);
}


/******************************** 主界面  ******************************/

void lv_main_page(void)
{
    lvgl_port_lock(0);

    lv_obj_del(lckfb_logo); // 删除开机logo
    // 创建主界面基本对象
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0); // 修改背景为黑色

    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_radius(&style, 10);
    lv_style_set_bg_opa( &style, LV_OPA_COVER );
    lv_style_set_bg_color(&style, lv_color_hex(0x00BFFF));
    lv_style_set_bg_grad_color( &style, lv_color_hex( 0x00BF00 ) );
    lv_style_set_bg_grad_dir( &style, LV_GRAD_DIR_VER );
    lv_style_set_border_width(&style, 0);
    lv_style_set_pad_all(&style, 0);
    lv_style_set_width(&style, 320);
    lv_style_set_height(&style, 240);

    main_obj = lv_obj_create(lv_scr_act());
    lv_obj_add_style(main_obj, &style, 0);

    // 显示左上角欢迎语
    main_text_label = lv_label_create(main_obj);
    lv_obj_set_style_text_font(main_text_label, &font_alipuhui20, 0);
    lv_label_set_long_mode(main_text_label, LV_LABEL_LONG_SCROLL_CIRCULAR);     /*Circular scroll*/
    lv_obj_set_width(main_text_label, 280);
    lv_label_set_text(main_text_label, "欢迎使用立创实战派开发板");
    lv_obj_align_to(main_text_label, main_obj, LV_ALIGN_TOP_LEFT, 8, 5);

    // 设置应用图标style
    static lv_style_t btn_style;
    lv_style_init(&btn_style);
    lv_style_set_radius(&btn_style, 16);
    lv_style_set_bg_opa( &btn_style, LV_OPA_COVER );
    lv_style_set_text_color(&btn_style, lv_color_hex(0xffffff));
    lv_style_set_border_width(&btn_style, 0);
    lv_style_set_pad_all(&btn_style, 5);
    lv_style_set_width(&btn_style, 80);
    lv_style_set_height(&btn_style, 80);

    // 创建第1个应用图标 音乐播放器
    lv_obj_t *icon1 = lv_btn_create(main_obj);
    lv_obj_add_style(icon1, &btn_style, 0);
    lv_obj_set_style_bg_color(icon1, lv_color_hex(0xf87c30), 0);
    lv_obj_set_pos(icon1, 65, 50);
    lv_obj_add_event_cb(icon1, music_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t * img1 = lv_img_create(icon1);
    LV_IMG_DECLARE(img_music_icon);
    lv_img_set_src(img1, &img_music_icon);
    lv_obj_align(img1, LV_ALIGN_CENTER, 0, 0);

    // 创建第2个应用图标 SD卡
    lv_obj_t *icon2 = lv_btn_create(main_obj);
    lv_obj_add_style(icon2, &btn_style, 0);
    lv_obj_set_style_bg_color(icon2, lv_color_hex(0x008b8b), 0);
    lv_obj_set_pos(icon2, 175, 50);
    lv_obj_add_event_cb(icon2, sdcard_event_handler, LV_EVENT_CLICKED, NULL);

    lv_obj_t * img2 = lv_img_create(icon2);
    LV_IMG_DECLARE(img_sd_icon);
    lv_img_set_src(img2, &img_sd_icon);
    lv_obj_align(img2, LV_ALIGN_CENTER, 0, 0);

    lvgl_port_unlock();
}

