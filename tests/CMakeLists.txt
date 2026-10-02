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
