# Additional clean files
cmake_minimum_required(VERSION 3.16)

if("${CONFIG}" STREQUAL "" OR "${CONFIG}" STREQUAL "")
  file(REMOVE_RECURSE
  "MatsuMonsterMesh.bin"
  "MatsuMonsterMesh.map"
  "badgeteam_ota.pem.S"
  "bootloader\\bootloader.bin"
  "bootloader\\bootloader.elf"
  "bootloader\\bootloader.map"
  "config\\sdkconfig.cmake"
  "config\\sdkconfig.h"
  "esp-idf\\esptool_py\\flasher_args.json.in"
  "esp-idf\\mbedtls\\x509_crt_bundle"
  "flash_app_args"
  "flash_bootloader_args"
  "flash_project_args"
  "flasher_args.json"
  "ldgen_libraries"
  "ldgen_libraries.in"
  "letsencrypt_isrg_root_x1.pem.S"
  "letsencrypt_isrg_root_x2.pem.S"
  "mch2022_ota.pem.S"
  "project_elf_src_esp32p4.c"
  "tanmatsu_apps.pem.S"
  "tanmatsu_ota.pem.S"
  "x509_crt_bundle.S"
  )
endif()
