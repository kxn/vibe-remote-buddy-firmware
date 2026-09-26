/* Private backup transactions share the binding commit and BLE task. */
static struct {
  char *bytes;
  uint32_t token, length, used, last, slot, peer, revision;
} snapshot_transfer;
static void snapshot_abort(void) {
  free(snapshot_transfer.bytes);
  memset(&snapshot_transfer, 0, sizeof snapshot_transfer);
}
static void snapshot_tick(uint32_t now, bool managed) {
  if (snapshot_transfer.token &&
      (!managed || (int32_t)(now - snapshot_transfer.last) > 15000))
    snapshot_abort();
}
static uint16_t snapshot_command(uint16_t op, const cJSON *q, cJSON *j) {
  uint32_t index, peer, revision, offset;
  if (op == 0x466 || op == 0x467) {
    if (!buddy_u32(q,"slot",&index) || index >= SLOTS ||
        !buddy_u32(q,"peer_id",&peer) || !buddy_u32(q,"revision",&revision))
      return RBP_STATUS_INVALID_ARGUMENT;
    slot_t *s = &slots[index];
    if (!peer || peer != s->record.peer_id || revision != s->record.map.revision)
      return RBP_STATUS_BAD_STATE;
    if (op == 0x466) {
      if (!buddy_u32(q,"offset",&offset)) return RBP_STATUS_INVALID_ARGUMENT;
      char *text = buddy_record_encode(&s->record);
      if (!text) return RBP_STATUS_RESOURCE_LIMIT;
      size_t length = strlen(text) + 1;
      if (offset >= length) {free(text);return RBP_STATUS_INVALID_ARGUMENT;}
      size_t n = length - offset;if(n>160)n=160;
      char hex[321];const char *digits="0123456789abcdef";
      for(size_t i=0;i<n;i++){unsigned b=(uint8_t)text[offset+i];hex[2*i]=digits[b>>4];hex[2*i+1]=digits[b&15];}
      hex[n*2]=0;free(text);
      cJSON_AddNumberToObject(j,"length",length);cJSON_AddStringToObject(j,"hex",hex);
      return 0;
    }
    uint32_t length;
    if(!buddy_u32(q,"length",&length)||!length||length>16384)return RBP_STATUS_INVALID_ARGUMENT;
    if(snapshot_transfer.token||standalone_busy()||operation.pending||buddy_probe_active()||buddy_catalog_store_busy()||buddy_update_busy()||!buddy_update_confirmed())return RBP_STATUS_BUSY;
    char *bytes=malloc(length);if(!bytes)return RBP_STATUS_RESOURCE_LIMIT;
    snapshot_transfer.bytes=bytes;snapshot_transfer.length=length;
    snapshot_transfer.token=esp_random()|1;snapshot_transfer.slot=index;snapshot_transfer.peer=peer;
    snapshot_transfer.revision=revision;snapshot_transfer.last=now_ms();
    cJSON_AddNumberToObject(j,"token",snapshot_transfer.token);return 0;
  }
  uint32_t token;
  if(!buddy_u32(q,"token",&token)||!snapshot_transfer.token||token!=snapshot_transfer.token)return RBP_STATUS_BAD_STATE;
  snapshot_transfer.last=now_ms();
  if(op==0x46a){snapshot_abort();return 0;}
  if(op==0x468){
    const char *hex=cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(q,"hex"));
    if(!hex||!strlen(hex)||strlen(hex)>320||strlen(hex)%2||!buddy_u32(q,"offset",&offset)||offset!=snapshot_transfer.used||strlen(hex)/2>snapshot_transfer.length-offset)return RBP_STATUS_INVALID_ARGUMENT;
    size_t n=strlen(hex)/2;
    for(size_t i=0;i<n;i++){
      const char *digits="0123456789abcdef";const char *a=strchr(digits,hex[2*i]),*b=strchr(digits,hex[2*i+1]);
      if(!a||!b)return RBP_STATUS_INVALID_ARGUMENT;
      snapshot_transfer.bytes[offset+i]=(char)(((a-digits)<<4)|(b-digits));
    }
    snapshot_transfer.used+=(uint32_t)n;return 0;
  }
  if(op!=0x469)return RBP_STATUS_UNSUPPORTED;
  slot_t *s=&slots[snapshot_transfer.slot];
  if(s->record.peer_id!=snapshot_transfer.peer||s->record.map.revision!=snapshot_transfer.revision)return RBP_STATUS_BAD_STATE;
  if(standalone_busy()||s->pairing||s->forgetting)return RBP_STATUS_BUSY;
  record_t *r=&transaction_record;
  if(snapshot_transfer.used!=snapshot_transfer.length||!buddy_record_decode(snapshot_transfer.bytes,snapshot_transfer.length,r)||r->peer_id!=s->record.peer_id||!buddy_address_equal(&r->address,&s->record.address)||strcmp(r->model_id,s->record.model_id)||!buddy_model_compatible(&r->definition,&s->record.definition))return RBP_STATUS_INVALID_ARGUMENT;
  /* Never import bonding identity, connection cache or a stale revision. */
  r->address=s->record.address;r->cache_len=s->record.cache_len;memcpy(r->cache,s->record.cache,sizeof r->cache);
  r->map.revision=s->record.map.revision+1;if(!r->map.revision)r->map.revision=1;
  if(!persist(s->model.slot,r))return RBP_STATUS_STORAGE_FAILED;
  s->record=*r;configure_model(s);s3_mapping(s->model.slot,&s->record.map);
  snapshot_abort();changed(s->model.slot);cJSON_AddNumberToObject(j,"revision",s->record.map.revision);return 0;
}
