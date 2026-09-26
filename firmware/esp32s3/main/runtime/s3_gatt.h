#pragma once
#include "rc003_adapter.h"
#define S3_GATT_SLOTS 5
typedef struct {
 bool complete,pnp_valid;
 uint8_t service_count,pnp[7];
 char services[32][37];
} s3_identity_t;
typedef struct s3_gatt s3_gatt_t;
struct s3_gatt {rc003_adapter_t *adapter;uint16_t connection,read_offset;uintptr_t operation;bool found;unsigned slot;uint16_t cached_handle,cached_offset;bool cached_long;
 s3_identity_t identity;unsigned identity_stage;uint32_t identity_deadline;
 void (*identity_done)(s3_gatt_t *,bool);
};
void s3_gatt_identify(s3_gatt_t *,void (*done)(s3_gatt_t *,bool));
void s3_gatt_init(s3_gatt_t *g,unsigned slot);
void s3_gatt_attach(s3_gatt_t *g,rc003_adapter_t *adapter,uint16_t connection);
void s3_gatt_detach(s3_gatt_t *g);
void s3_gatt_notify(s3_gatt_t *g,uint16_t handle,const uint8_t *data,uint16_t len);
const rbp_gatt_client_t *s3_gatt_client(void);

#ifdef S3_HCI_PROBE
void s3_link_trace(unsigned slot,unsigned event,uint32_t a,uint32_t b,uint32_t c);
#else
#define s3_link_trace(...) ((void)0)
#endif

void s3_gatt_poll(s3_gatt_t *g);
