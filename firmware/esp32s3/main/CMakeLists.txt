option(S3_USB_QUALIFY "Only silence and all-up reports; no Bluetooth" OFF)
option(S3_HCI_PROBE "Record bounded RX headers for packet-loss diagnosis" OFF)
option(S3_CODEC_METRICS "Expose voice decoder timing and stack metrics" OFF)
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

set(SOURCES
    "app/main.c"
    "app/usb_device.c"
    "app/host_os.c"
    "app/buddy_power.c"
)
if(NOT S3_USB_QUALIFY)
    list(APPEND SOURCES
        "management/buddy_bond_storage.c"
        "catalog/buddy_catalog.c"
        "catalog/buddy_catalog_query.c"
        "catalog/buddy_catalog_store.c"
        "catalog/buddy_model_wire.c"
        "catalog/buddy_input.c"
        "catalog/buddy_models.c"
        "catalog/buddy_models_builtin.c"
        "runtime/s3_runtime.c"
        "runtime/s3_bridge.c"
        "runtime/s3_gatt.c"
        "runtime/s3_arbiter.c"
        "management/buddy_management.c"
        "management/buddy_record.c"
        "management/buddy_probe.c"
        "management/buddy_update.c"
        "${ROOT}/protocol/src/rbp_frame.c"
        "${ROOT}/protocol/src/rbp_tlv.c"
        "${ROOT}/firmware/product/faults.c"
        "${ROOT}/firmware/product/device_model.c"
        "${ROOT}/firmware/adapters/hogp/report_map.c"
        "${ROOT}/firmware/adapters/rc003/rc003_adapter.c"
        "${ROOT}/firmware/adapters/rc003/rc003_atvv.c"
        "${ROOT}/firmware/codecs/ima_decoder.c"
        "${ROOT}/firmware/codecs/rbp_decoder.c"
        "${ROOT}/firmware/codecs/ico_cleanroom/ico_cleanroom.c"
    )
endif()

idf_component_register(SRCS ${SOURCES}
    INCLUDE_DIRS
        "." "app" "catalog" "management" "runtime"
        "${ROOT}/firmware/debug"
        "${ROOT}/firmware/product"
        "${ROOT}/firmware/adapters/rc003"
        "${ROOT}/protocol/include"
        "${ROOT}/firmware/codecs"
        "${ROOT}/firmware/codecs/ico_cleanroom"
    REQUIRES esp_tinyusb esp_timer esp_psram esp_pm esp_driver_tsens usb bt nvs_flash json app_update esp_app_format spi_flash mbedtls bootloader_support)
if(NOT CMAKE_SCRIPT_MODE_FILE AND NOT S3_USB_QUALIFY)
    set(ICO_TABLE_DIR "${CMAKE_BINARY_DIR}/generated/ico_cleanroom")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_command(
        OUTPUT "${ICO_TABLE_DIR}/itu_g7221_numeric_tables.h" "${ICO_TABLE_DIR}/ico_huffman_lookup.h"
        COMMAND "${Python3_EXECUTABLE}" "${ROOT}/firmware/codecs/ico_cleanroom/fetch_itu_tables.py"
                --output "${ICO_TABLE_DIR}"
        DEPENDS "${ROOT}/firmware/codecs/ico_cleanroom/fetch_itu_tables.py"
                "${ROOT}/firmware/codecs/ico_cleanroom/generate_huffman_lookup.py"
        COMMENT "Extracting local G.722.1 tables from the verified ITU package"
        VERBATIM)
    add_custom_target(ico_local_tables DEPENDS
        "${ICO_TABLE_DIR}/itu_g7221_numeric_tables.h" "${ICO_TABLE_DIR}/ico_huffman_lookup.h")
    add_dependencies(${COMPONENT_LIB} ico_local_tables)
    target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE "${ICO_TABLE_DIR}")
endif()
if(S3_USB_QUALIFY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE S3_USB_QUALIFY)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RBP_STANDALONE RBP_MULTI_STANDALONE RBP_ATT_MTU=247)
endif()
if(S3_CODEC_METRICS AND NOT S3_USB_QUALIFY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE S3_CODEC_METRICS)
endif()
foreach(component espressif__tinyusb espressif__esp_tinyusb main)
    idf_component_get_property(lib ${component} COMPONENT_LIB)
    target_compile_options(${lib} PRIVATE "-include" "${CMAKE_CURRENT_LIST_DIR}/app/audio_config.h")
endforeach()

target_compile_options(${COMPONENT_LIB} PRIVATE -fwrapv)

if(S3_HCI_PROBE AND NOT S3_USB_QUALIFY)
    target_sources(${COMPONENT_LIB} PRIVATE "${ROOT}/firmware/debug/hci_probe.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE S3_HCI_PROBE)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_vhci_host_register_callback" "-Wl,--wrap=ble_gap_update_params")
endif()

target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dcd_event_handler")
if(NOT S3_USB_QUALIFY)
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=nvs_open")
    foreach(operation our_secs peer_secs cccds rpa_recs)
        target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_store_config_persist_${operation}")
    endforeach()
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_store_persist_peer_records")
endif()

if(NOT S3_USB_QUALIFY)
    set(MSBC "$ENV{IDF_PATH}/components/bt/host/bluedroid/external/sbc/decoder")
    file(GLOB MSBC_SOURCES "${MSBC}/srce/*.c")
    target_sources(${COMPONENT_LIB} PRIVATE "${ROOT}/firmware/codecs/msbc.c" ${MSBC_SOURCES})
    target_include_directories(${COMPONENT_LIB} BEFORE PRIVATE "${ROOT}/firmware/codecs/msbc_shim" "${ROOT}/firmware/codecs" "${MSBC}/include")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE RBP_WITH_MSBC)
endif()
