idf_component_register(
  REQUIRES driver
           nvs_flash
  SRCS "ssd1619.c"
       "ssd1619_lut.c"
  INCLUDE_DIRS include
)
