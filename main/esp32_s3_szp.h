#pragma once

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_lvgl_port.h"

#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/i2s_std.h"

#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"


/******************************************************************************/
/***************************  I2C ↓ *******************************************/
#define BSP_I2C_SDA           (GPIO_NUM_1)   // SDA引脚
#define BSP_I2C_SCL           (GPIO_NUM_2)   // SCL引脚

#define BSP_I2C_NUM           (0)            // I2C外设
#define BSP_I2C_FREQ_HZ       100000         // 100kHz

esp_err_t bsp_i2c_init(void);   // 初始化I2C接口
/***************************  I2C ↑  *******************************************/
/*******************************************************************************/


/******************************************************************************/
/***************************   I2S  ↓    **************************************/

/* Example configurations */
#define EXAMPLE_RECV_BUF_SIZE   (2400)
#define EXAMPLE_SAMPLE_RATE     (16000)
#define EXAMPLE_MCLK_MULTIPLE   (384) // If not using 24-bit data width, 256 should be enough
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define EXAMPLE_VOICE_VOLUME    (70)

/* I2S port and GPIOs */
#define I2S_NUM         (0)
#define I2S_MCK_IO      (GPIO_NUM_38)
#define I2S_BCK_IO      (GPIO_NUM_14)
#define I2S_WS_IO       (GPIO_NUM_13)
#define I2S_DO_IO       (GPIO_NUM_45)
#define I2S_DI_IO       (-1)


/***********************************************************/
/***************    IO扩展芯片 ↓   *************************/
#define PCA9557_INPUT_PORT              0x00
#define PCA9557_OUTPUT_PORT             0x01
#define PCA9557_POLARITY_INVERSION_PORT 0x02
#define PCA9557_CONFIGURATION_PORT      0x03

#define LCD_CS_GPIO                 BIT(0)    // PCA9557_GPIO_NUM_1
#define PA_EN_GPIO                  BIT(1)    // PCA9557_GPIO_NUM_2

#define PCA9557_SENSOR_ADDR             0x19        /*!< Slave address of the MPU9250 sensor */

#define SET_BITS(_m, _s, _v)  ((_v) ? (_m)|((_s)) : (_m)&~((_s)))

void pca9557_init(void);
void lcd_cs(uint8_t level);
void pa_en(uint8_t level);
/***************    IO扩展芯片 ↑   *************************/
/***********************************************************/


/***********************************************************/
/****************    LCD显示屏 ↓   *************************/
#define BSP_LCD_PIXEL_CLOCK_HZ     (80 * 1000 * 1000)
#define BSP_LCD_SPI_NUM            (SPI3_HOST)
#define LCD_CMD_BITS               (8)
#define LCD_PARAM_BITS             (8)
#define BSP_LCD_BITS_PER_PIXEL     (16)
#define LCD_LEDC_CH          LEDC_CHANNEL_0

#define BSP_LCD_H_RES              (320)
#define BSP_LCD_V_RES              (240)

#define BSP_LCD_SPI_MOSI      (GPIO_NUM_40)
#define BSP_LCD_SPI_CLK       (GPIO_NUM_41)
#define BSP_LCD_SPI_CS        (GPIO_NUM_NC)
#define BSP_LCD_DC            (GPIO_NUM_39)
#define BSP_LCD_RST           (GPIO_NUM_NC)
#define BSP_LCD_BACKLIGHT     (GPIO_NUM_42)

#define BSP_LCD_DRAW_BUF_HEIGHT    (20)

esp_err_t bsp_display_brightness_init(void);
esp_err_t bsp_display_brightness_set(int brightness_percent);
esp_err_t bsp_display_backlight_off(void);
esp_err_t bsp_display_backlight_on(void);
esp_err_t bsp_lcd_init(void);
void lcd_set_color(uint16_t color);
void lcd_draw_pictrue(int x_start, int y_start, int x_end, int y_end, const unsigned char *gImage);
void bsp_lvgl_start(void);
/***************    LCD显示屏 ↑   *************************/
/***********************************************************/


/***********************************************************/
/************  SPI Flash FATFS文件系统 ↓  ******************/
#define FLASH_FATFS_BASE        "/flashfs"   // SPI Flash FATFS 挂载路径

esp_err_t bsp_flash_fatfs_mount(void);  // 挂载SPI Flash上的FATFS分区(磨损均衡)

/************  SPI Flash FATFS文件系统 ↑  ******************/
/**********************************************************/


/***********************************************************/
/**********************    SD卡 ↓   *********************/
/* SD card GPIOs */
#define SD_CMD_IO      (48)
#define SD_CLK_IO      (47)
#define SD_DAT0_IO     (21)

#define SD_MOUNT_POINT     "/sdcard"

esp_err_t bsp_sdcard_mount(void); // 挂载SD卡
esp_err_t bsp_sdcard_unmount(void); // 卸载SD卡
/**********************    SD卡 ↑  *********************/
/**********************************************************/


/***********************************************************/
/*********************    音频 ↓   *************************/

#define VOLUME_DEFAULT    60        // 默认声音大小 0~100

#define CODEC_DEFAULT_SAMPLE_RATE          (16000)
#define CODEC_DEFAULT_BIT_WIDTH            (16)
#define CODEC_DEFAULT_ADC_VOLUME           (24.0)
#define CODEC_DEFAULT_CHANNEL              (2)

#define BSP_I2S_NUM                  I2S_NUM_1

#define GPIO_I2S_LRCK       (GPIO_NUM_13)
#define GPIO_I2S_MCLK       (GPIO_NUM_38)
#define GPIO_I2S_SCLK       (GPIO_NUM_14)
#define GPIO_I2S_SDIN       (GPIO_NUM_12)
#define GPIO_I2S_DOUT       (GPIO_NUM_45)
#define GPIO_PWR_CTRL       (GPIO_NUM_NC)

esp_err_t bsp_codec_init(void);
esp_err_t bsp_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms);
esp_err_t bsp_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);
esp_err_t bsp_speaker_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);
esp_err_t bsp_codec_mute_set(bool enable);
esp_err_t bsp_codec_volume_set(int volume, int *volume_set);

#define START_MUSIC_COMPLETED            BIT0
void power_music_task(void *pvParameters);

/*********************    音频 ↑   *************************/
/***********************************************************/
