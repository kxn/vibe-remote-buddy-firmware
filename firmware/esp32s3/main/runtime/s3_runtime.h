#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
void s3_runtime_init(void);
void s3_bridge_start(void);
size_t s3_cdc_receive(const uint8_t *data,size_t len);
size_t s3_cdc_output(uint8_t *data,size_t len);
void s3_cdc_state(bool connected);
uint32_t s3_cdc_epoch(void);
void s3_usb_state(bool mounted,bool recording);
void s3_hid_desired(uint8_t report[8]);
uint16_t s3_consumer_desired(void);
uint32_t s3_audio_epoch(void);
void s3_hid_fault(void);
unsigned s3_pcm_read(int16_t *pcm,unsigned samples);

#ifdef RBP_MULTI_STANDALONE
#include "buddy_input.h"
#include "rbp_server.h"
void s3_peer_link(unsigned slot,uint32_t generation);
void s3_peer_disconnected(unsigned slot,uint32_t now);
void s3_peer_voice_key(unsigned slot,uint32_t generation,bool down,uint32_t now);
bool s3_peer_wanted(unsigned slot,uint32_t generation);
void s3_peer_voice(unsigned slot,uint32_t generation,const rbp_voice_evt_t *event,uint32_t now);
uint64_t s3_peer_keys(unsigned slot,uint32_t generation,uint64_t logical);
void s3_peer_abort(unsigned slot,uint32_t generation);
void s3_voice_mode_reset(unsigned slot);
void s3_mapping(unsigned slot,const buddy_map_t *map);
void s3_management(bool active);
unsigned s3_voice_owner(void);
uint8_t s3_take_stops(void);
bool s3_peer_rejected(unsigned slot);
#endif

void s3_probe_audio_reset(void);
unsigned s3_probe_audio_state(uint32_t *samples,uint32_t *pending);
