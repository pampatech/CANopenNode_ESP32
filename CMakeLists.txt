set(srcs "")
set(include_dirs "")
set(private_include_dirs "")
set(private_requirements "")
set(ldfragments "")
set(co_dir "CANopenNode")
set(co_port_dir "port")

if(CONFIG_USE_CANOPENNODE)
  # Add common source files
  list(APPEND srcs
    "CANopenNode_ESP32.c"
    "${co_dir}/CANopen.c"
    "${co_dir}/301/CO_Emergency.c"
    "${co_dir}/301/CO_fifo.c"
    "${co_dir}/301/CO_HBconsumer.c"
    "${co_dir}/301/CO_NMT_Heartbeat.c"
    "${co_dir}/301/CO_ODinterface.c"
    "${co_dir}/301/CO_PDO.c"
    "${co_dir}/301/CO_SDOserver.c"
    "${co_dir}/301/CO_SYNC.c"
    "${co_dir}/301/CO_TIME.c"
    "${co_dir}/305/CO_LSSmaster.c"
    "${co_dir}/305/CO_LSSslave.c"
    "${co_port_dir}/CO_driver.c")
  list(APPEND include_dirs
    "."
    "${co_dir}"
    "${co_port_dir}")
endif() #CONFIG_USE_CANOPENNODE

if(CONFIG_CO_SDO_CLIENT_ENABLE)
  list(APPEND srcs
  "${co_dir}/301/CO_SDOclient.c")
endif() #CONFIG_CO_SDO_CLIENT_ENABLE 

if(CONFIG_CO_LED_ENABLE)
  list(APPEND srcs
    "${co_dir}/303/CO_LEDs.c")
endif() #CONFIG_CO_LED_ENABLE

if (CONFIG_CO_SDO_SERVER_BLOCK_TRANSFER OR CONFIG_CO_SDO_CLIENT_BLOCK_TRANSFER)
list(APPEND srcs
  "${co_dir}/301/crc16-ccitt.c")
endif() #CONFIG_CO_SDO_SERVER_BLOCK_TRANSFER
 
if(CONFIG_CO_STORAGE)
list(APPEND srcs
  "${co_dir}/storage/CO_storage.c"
  "${co_port_dir}/CO_nvs.c")
endif()

if(CONFIG_CO_OTA_ENABLE)
list(APPEND srcs
  "extra/can_ota/can_ota.c")
list(APPEND include_dirs
  "extra/can_ota")
list(APPEND private_requirements
  app_update esp_timer)
endif() #CONFIG_CO_OTA_ENABLE

if(CONFIG_CO_CONSOLE_ENABLE)
list(APPEND srcs
  "extra/co_console/co_console.c"
  "extra/co_console/log_ringbuffer.c")
list(APPEND include_dirs
  "extra/co_console")
list(APPEND private_requirements
  console vfs esp_driver_uart)
if(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
list(APPEND private_requirements
  esp_driver_usb_serial_jtag)
endif()
endif() #CONFIG_CO_CONSOLE_ENABLE

idf_component_register(SRCS ${srcs}
                    INCLUDE_DIRS ${include_dirs}
                    PRIV_INCLUDE_DIRS ${private_include_dirs}
                    LDFRAGMENTS ${ldfragments}
                    PRIV_REQUIRES ${private_requirements}
                    REQUIRES freertos driver esp_driver_twai main)
