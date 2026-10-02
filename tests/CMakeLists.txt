cmake_minimum_required(VERSION 3.16)
project(buddy_adapter_tests C)
enable_testing()
add_executable(protocol_mode_test
    protocol_mode_test.c
    ../firmware/adapters/rc003/rc003_atvv.c
    ../firmware/adapters/hogp/report_map.c
    ../firmware/product/device_model.c
    ../firmware/product/faults.c
    ../protocol/src/rbp_frame.c)
target_compile_features(protocol_mode_test PRIVATE c_std_11)
target_compile_definitions(protocol_mode_test PRIVATE RBP_ATT_MTU=247)
target_include_directories(protocol_mode_test PRIVATE
    ../firmware/product ../protocol/include)
add_test(NAME protocol_mode COMMAND protocol_mode_test)

add_executable(host_hid_test
    host_hid_test.c
    ../firmware/esp32s3/main/app/host_os.c
    ../firmware/esp32s3/main/catalog/buddy_input.c
    ../firmware/esp32s3/main/catalog/buddy_shortcuts.c
    ../firmware/esp32s3/main/runtime/s3_arbiter.c
    ../firmware/adapters/hogp/report_map.c)
target_compile_features(host_hid_test PRIVATE c_std_11)
target_include_directories(host_hid_test PRIVATE
    ../firmware/esp32s3/main/app
    ../firmware/esp32s3/main/catalog
    ../firmware/esp32s3/main/runtime
    ../firmware/adapters/hogp
    ../firmware/product ../protocol/include)
add_test(NAME host_hid COMMAND host_hid_test)

add_executable(shortcut_test shortcut_test.c
    ../firmware/esp32s3/main/catalog/buddy_input.c
    ../firmware/esp32s3/main/catalog/buddy_shortcuts.c
    ../firmware/esp32s3/main/runtime/s3_arbiter.c)
target_compile_features(shortcut_test PRIVATE c_std_11)
target_include_directories(shortcut_test PRIVATE
    ../firmware/esp32s3/main/app ../firmware/esp32s3/main/catalog
    ../firmware/esp32s3/main/runtime ../firmware/product ../protocol/include)
add_test(NAME shortcut COMMAND shortcut_test)

add_executable(shortcut_store_test shortcut_store_test.c
    ../firmware/esp32s3/main/management/buddy_shortcut_store.c
    ../firmware/esp32s3/main/catalog/buddy_shortcuts.c)
target_compile_features(shortcut_store_test PRIVATE c_std_11)
target_include_directories(shortcut_store_test PRIVATE nvs_stub
    ../firmware/esp32s3/main/app ../firmware/esp32s3/main/catalog
    ../firmware/esp32s3/main/management ../protocol/include)
add_test(NAME shortcut_store COMMAND shortcut_store_test)

add_executable(voice_runtime_test voice_runtime_test.c
    ../firmware/esp32s3/main/runtime/s3_runtime.c
    ../firmware/esp32s3/main/runtime/s3_arbiter.c
    ../firmware/esp32s3/main/catalog/buddy_input.c
    ../firmware/esp32s3/main/catalog/buddy_shortcuts.c
    ../firmware/product/faults.c)
target_compile_features(voice_runtime_test PRIVATE c_std_11)
target_compile_definitions(voice_runtime_test PRIVATE RBP_MULTI_STANDALONE)
target_include_directories(voice_runtime_test PRIVATE runtime_stub
    ../firmware/esp32s3/main/app ../firmware/esp32s3/main/catalog
    ../firmware/esp32s3/main/runtime ../firmware/product
    ../firmware/codecs ../firmware/codecs/ico_cleanroom ../protocol/include)
add_test(NAME voice_runtime COMMAND voice_runtime_test)
