idf_component_register(
    SRCS "src/bl0937.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_timer freertos
)
