/* RBP/3.0 shared definitions (wire protocol).
 *
 * Matches docs/wire-protocol.md (design v0.3).  Portable C99, no OS deps.
 */
#ifndef RBP_DEFS_H
#define RBP_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBP_MAGIC0 0x52 /* 'R' */
#define RBP_MAGIC1 0x42 /* 'B' */
#define RBP_MAJOR  3
#define RBP_MINOR  0

#define RBP_HEADER_SIZE  32u
#define RBP_MAX_PAYLOAD  512u
/* header + payload (no CRC): 544 */
#define RBP_MAX_DECODED  (RBP_HEADER_SIZE + RBP_MAX_PAYLOAD)
/* header + payload + CRC32C: 548 */
#define RBP_MAX_FRAME_RAW (RBP_MAX_DECODED + 4u)
/* worst-case wire bytes incl. COBS overhead and 0x00 delimiter: 553.
 * measured worst COBS overhead for a 548-byte raw frame is 4. */
#define RBP_MAX_ENCODED  552u
#define RBP_COBS_RECV_MAX 551u

#define RBP_KIND_REQUEST 1u
#define RBP_KIND_RESPONSE 2u
#define RBP_KIND_EVENT 3u
#define RBP_KIND_AUDIO 4u

/* Response status codes */
#define RBP_STATUS_OK 0u
#define RBP_STATUS_ACCEPTED 1u
#define RBP_STATUS_INVALID_ARGUMENT 2u
#define RBP_STATUS_UNSUPPORTED 3u
#define RBP_STATUS_BAD_STATE 4u
#define RBP_STATUS_BUSY 5u
#define RBP_STATUS_NOT_FOUND 6u
#define RBP_STATUS_TIMEOUT 7u
#define RBP_STATUS_PAIRING_FAILED 8u
#define RBP_STATUS_LINK_LOST 9u
#define RBP_STATUS_STORAGE_FAILED 10u
#define RBP_STATUS_RESOURCE_LIMIT 11u
#define RBP_STATUS_CANCELLED 12u
#define RBP_STATUS_DUPLICATE 13u
#define RBP_STATUS_SESSION_MISMATCH 14u
#define RBP_STATUS_DEVICE_ERROR 15u
#define RBP_STATUS_VOICE_UNAVAILABLE 16u
#define RBP_STATUS_VERSION_MISMATCH 17u

/* Opcodes: receiver level */
#define RBP_OP_HELLO 0x0001u
#define RBP_OP_PING 0x0002u
#define RBP_OP_GET_DEVICE 0x0003u
#define RBP_OP_GET_PEER 0x0004u
#define RBP_OP_GET_OPERATION 0x0005u
#define RBP_OP_GET_STATS 0x0006u
#define RBP_OP_GOODBYE 0x0007u

/* Opcodes: find & pairing */
#define RBP_OP_FIND_START 0x0100u
#define RBP_OP_FIND_LIST 0x0101u
#define RBP_OP_FIND_STOP 0x0102u
#define RBP_OP_PAIR_BEGIN 0x0110u
#define RBP_OP_PAIR_REPLY 0x0111u
#define RBP_OP_PAIR_CANCEL 0x0112u
#define RBP_OP_FORGET_PEER 0x0113u
#define RBP_OP_SET_RECONNECT 0x0114u
#define RBP_OP_CONNECT_PEER 0x0115u
#define RBP_OP_DISCONNECT 0x0116u

/* Opcodes: keys */
#define RBP_OP_KEY_CATALOG 0x0200u
#define RBP_OP_KEYS_SNAPSHOT 0x0201u
#define RBP_OP_EVENTS_ENABLE 0x0202u
#define RBP_OP_KEYS_STATE_EV 0x0280u /* event */

/* Opcodes: voice */
#define RBP_OP_VOICE_ENABLE 0x0300u
#define RBP_OP_VOICE_STOP 0x0301u
#define RBP_OP_VOICE_START 0x0303u
#define RBP_OP_VOICE_STARTED_EV 0x0380u
#define RBP_OP_VOICE_DATA 0x0381u /* kind=4 */
#define RBP_OP_VOICE_ENDED_EV 0x0382u
#define RBP_OP_VOICE_FORMAT_EV 0x0383u

/* Opcodes: events (pairing / device state) */
#define RBP_OP_FIND_DONE_EV 0x0180u
#define RBP_OP_PAIR_PROMPT_EV 0x0181u
#define RBP_OP_OPERATION_EV 0x0182u
#define RBP_OP_DEVICE_STATE_EV 0x0183u

/* TLV value types */
#define RBP_TLV_T_U8 1u
#define RBP_TLV_T_U16 2u
#define RBP_TLV_T_U32 3u
#define RBP_TLV_T_U64 4u
#define RBP_TLV_T_BOOL 5u
#define RBP_TLV_T_TEXT 6u
#define RBP_TLV_T_BYTES 7u

/* Features bits (HELLO) */
#define RBP_FEAT_ENCODED_VOICE 0x01u
#define RBP_FEAT_INTERACTIVE_PAIR 0x02u
#define RBP_FEAT_BOND_SAVE 0x04u
#define RBP_FEAT_AUTO_RECONNECT 0x08u

/* DeviceInfo.state */
#define RBP_DEVSTATE_UNBOUND 0u
#define RBP_DEVSTATE_DISCONNECTED 1u
#define RBP_DEVSTATE_CONNECTING 2u
#define RBP_DEVSTATE_PAIRING 3u
#define RBP_DEVSTATE_INITIALIZING 4u
#define RBP_DEVSTATE_READY 5u
#define RBP_DEVSTATE_UNSUPPORTED 6u
#define RBP_DEVSTATE_ERROR 7u

/* DeviceInfo.voice_state */
#define RBP_VOICE_ABSENT 0u
#define RBP_VOICE_INITIALIZING 1u
#define RBP_VOICE_READY 2u
#define RBP_VOICE_UNSUPPORTED 3u
#define RBP_VOICE_FAILED 4u

/* DeviceInfo.voice_interaction */
#define RBP_VI_UNAVAILABLE 0u
#define RBP_VI_ON_REQUEST 1u
#define RBP_VI_PTT 2u
#define RBP_VI_HTT 3u

/* PAIR_PROMPT method */
#define RBP_PROMPT_CONFIRM_PAIR 0u
#define RBP_PROMPT_ENTER_PASSKEY 1u
#define RBP_PROMPT_CONFIRM_NUMBER 2u
#define RBP_PROMPT_DISPLAY_PASSKEY 3u

/* FIND_DONE reason */
#define RBP_FIND_DONE_EXPIRED 0u
#define RBP_FIND_DONE_USER_STOP 1u
#define RBP_FIND_DONE_PAIR_TAKEOVER 2u

/* KEYS_STATE structure */
#define RBP_KEYS_STRUCT_SIZE 24u
#define RBP_KEYS_KIND_SNAPSHOT 0u
#define RBP_KEYS_KIND_PHYSICAL 1u
#define RBP_KEYS_KIND_RESET 2u
#define RBP_KEYS_REASON_NONE 0u
#define RBP_KEYS_REASON_LINK_LOST 1u
#define RBP_KEYS_REASON_OVERFLOW 2u
#define RBP_KEYS_REASON_ADAPTER_ERROR 3u

/* VOICE_ENDED reason */
#define RBP_END_NORMAL 0u
#define RBP_END_CONSUMER_DISABLED 1u
#define RBP_END_LINK_LOST 2u
#define RBP_END_BUFFER_OVERRUN 3u
#define RBP_END_INVALID_ENCODED_DATA 4u
#define RBP_END_SOURCE_DATA_LOST 5u
#define RBP_END_DEVICE_ERROR 6u
#define RBP_END_REQUESTED_STOP 7u
#define RBP_END_CAPTURE_LIMIT 8u

/* Operation state */
#define RBP_OPSTATE_PENDING 0u
#define RBP_OPSTATE_COMPLETED 1u

/* Error TLV tags (responses) */
#define RBP_TAG_MESSAGE 1u
#define RBP_TAG_UNCERTAIN 2u

/* Max capacities (this implementation) */
#define RBP_MAX_KEYS 64u
#define RBP_MAX_CANDIDATES 8u
#define RBP_MAX_OPERATION_RESULTS 4u
#define RBP_KEY_QUEUE_DEPTH 8u
#define RBP_MAX_VOICE_BLOCK_SAMPLES 246u

/* Well-known key ids */
#define RBP_KEY_POWER 0x0001u
#define RBP_KEY_VOICE 0x0002u
#define RBP_KEY_UP 0x0003u
#define RBP_KEY_DOWN 0x0004u
#define RBP_KEY_LEFT 0x0005u
#define RBP_KEY_RIGHT 0x0006u
#define RBP_KEY_OK 0x0007u
#define RBP_KEY_BACK 0x0008u
#define RBP_KEY_HOME 0x0009u
#define RBP_KEY_MENU 0x000Au
#define RBP_KEY_TV 0x000Bu
#define RBP_KEY_VOLUME_UP 0x000Cu
#define RBP_KEY_VOLUME_DOWN 0x000Du
#define RBP_KEY_MUTE 0x000Eu
#define RBP_KEY_PLAY_PAUSE 0x000Fu
#define RBP_KEY_NEXT 0x0010u
#define RBP_KEY_PREVIOUS 0x0011u
#define RBP_KEY_STOP 0x0012u
#define RBP_KEY_SETTINGS 0x0013u
#define RBP_KEY_LOCAL 0x0014u
#define RBP_KEY_CHANNEL_UP 0x0015u
#define RBP_KEY_CHANNEL_DOWN 0x0016u
#define RBP_KEY_DIGIT_0 0x0017u
#define RBP_KEY_DIGIT_1 0x0018u
#define RBP_KEY_DIGIT_2 0x0019u
#define RBP_KEY_DIGIT_3 0x001Au
#define RBP_KEY_DIGIT_4 0x001Bu
#define RBP_KEY_DIGIT_5 0x001Cu
#define RBP_KEY_DIGIT_6 0x001Du
#define RBP_KEY_DIGIT_7 0x001Eu
#define RBP_KEY_DIGIT_8 0x001Fu
#define RBP_KEY_DIGIT_9 0x0020u
#define RBP_KEY_STAR 0x0021u
#define RBP_KEY_HASH 0x0022u



#define RBP_END_UNSUPPORTED_FORMAT 9u
#define RBP_OP_GET_VOICE_CAPS 0x0302u

#ifdef __cplusplus
}
#endif
#endif /* RBP_DEFS_H */
