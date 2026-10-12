idf_component_register(SRCS "esp32_s3_szp.c" "main.c" "app_ui.c" "audio_pipeline.c"
                       SRCS "assets/image_lckfb_logo.c"
                       SRCS "assets/img_music_icon.c"
                       SRCS "assets/img_sd_icon.c"
                       SRCS "assets/font_alipuhui20.c"
                       EMBED_FILES "sword.pcm"
                       INCLUDE_DIRS ".")

# 将 flash_fatfs 目录生成带磨损均衡的FATFS镜像, 烧录到storage分区
fatfs_create_spiflash_image(storage ../flash_fatfs FLASH_IN_PROJECT)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)
