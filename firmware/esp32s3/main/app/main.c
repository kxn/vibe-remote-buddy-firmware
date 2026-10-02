#include "buddy_power.h"
#ifndef S3_USB_QUALIFY
#include "s3_runtime.h"
#include "buddy_update.h"
#endif
void usb_device_start(void);
void app_main(void) {
    buddy_power_init();
#ifndef S3_USB_QUALIFY
    buddy_update_init();
    s3_runtime_init();
#endif
    usb_device_start();
#ifndef S3_USB_QUALIFY
    s3_bridge_start();
#endif
}
