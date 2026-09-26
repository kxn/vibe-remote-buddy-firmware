#include "report_map.h"
#include "rbp/defs.h"
#include <string.h>
/* Only bounded absolute button/keyboard layouts are supported. Unsupported
 * structures are rejected instead of inferred from report byte length. */
typedef struct {uint32_t page,size,count,id,lmin,lmax;} globals_t;
bool hogp_report_map_parse(const uint8_t *d,size_t len,hogp_report_map_t *out) {
    if(!d || !out || !len || len>HOGP_MAX_MAP_SIZE)return false;
    memset(out,0,sizeof *out);globals_t g={0},stack[4];unsigned sp=0,depth=0;
    uint32_t usages[32],umin=0,umax=0;unsigned nu=0;bool have_min=false,have_max=false;
    for(size_t pos=0;pos<len;) {
        uint8_t prefix=d[pos++];if(prefix==0xfe)return false;
        unsigned n=prefix&3;if(n==3)n=4;if(pos+n>len)return false;
        uint32_t v=0;for(unsigned i=0;i<n;i++)v|=(uint32_t)d[pos++]<<(8*i);
        unsigned type=(prefix>>2)&3,tag=prefix>>4;
        if(type==1) {
            switch(tag) {
            case 0:if(v>65535)return false;g.page=v;break;
            case 1:g.lmin=v;break;case 2:g.lmax=v;break;
            case 7:g.size=v;break;case 8:if(!v || v>255)return false;g.id=v;out->uses_report_ids=true;break;
            case 9:g.count=v;break;
            case 10:if(sp==4)return false;stack[sp++]=g;break;
            case 11:if(!sp)return false;g=stack[--sp];break;
            default:break;
            }
        } else if(type==2) {
            if(tag==0) {if(nu==32)return false;usages[nu++]=n==4?v:(g.page<<16)|v;}
            else if(tag==1) {umin=n==4?v:(g.page<<16)|v;have_min=true;}
            else if(tag==2) {umax=n==4?v:(g.page<<16)|v;have_max=true;}
            else if(tag==10)return false; /* delimiter sets not implemented */
        } else if(type==0) {
            if(tag==10) {if(++depth>16)return false;}
            else if(tag==12) {if(!depth)return false;depth--;}
            else if(tag==8) {
                /* Opaque vendor inputs (e.g. RC003's 120-byte audio reports)
                 * are not buttons. Keep their bit offsets until the whole Map
                 * is parsed, so a mixed Report ID cannot shift later fields. */
                bool vendor=g.page>=0xff00;
                if(have_min && (umin>>16)<0xff00)vendor=false;
                for(unsigned i=0;i<nu;i++)if((usages[i]>>16)<0xff00)vendor=false;
                if(!g.size || g.size>65535 || !g.count || g.count>65535)return false;
                if(!vendor && (g.size>16 || g.count>64))return false;
                hogp_input_report_t *r=NULL;
                for(unsigned i=0;i<out->input_count;i++)if(out->inputs[i].report_id==g.id)r=&out->inputs[i];
                if(!r) {if(out->input_count==HOGP_MAX_INPUT_REPORTS)return false;
                    r=&out->inputs[out->input_count++];r->report_id=g.id;r->usage_page=g.page;}
                uint32_t width=g.size*g.count;
                if(width>65535u-r->bits)return false;
                uint32_t end=r->bits+width;
                if(!(v&1) && !vendor && !(v&4)) {
                    if(end>512)return false;
                    if(v&0x100)return false; /* buffered bytes */
                    if(have_min!=have_max || (have_min && (umax<umin || (umax>>16)!=(umin>>16))))return false;
                    if(v&2) {
                        if(!nu && !have_min)return false;
                        for(unsigned j=0;j<g.count;j++) {
                            uint32_t usage=nu?usages[j<nu?j:nu-1]:umin+(j<=umax-umin?j:umax-umin);
                            if(out->field_count==HOGP_MAX_FIELDS)return false;
                            hogp_field_t *f=&out->fields[out->field_count++];
                            f->report_id=g.id;f->offset=r->bits+j*g.size;f->size=g.size;f->count=1;
                            f->page=usage>>16;f->usage_min=f->usage_max=usage&65535;f->variable=true;
                        }
                    } else {
                        if(!have_min || g.lmin>65535 || g.lmax>65535 || g.lmax<g.lmin)return false;
                        if(out->field_count==HOGP_MAX_FIELDS)return false;
                        hogp_field_t *f=&out->fields[out->field_count++];f->report_id=g.id;
                        f->offset=r->bits;f->size=g.size;f->count=g.count;f->page=umin>>16;
                        f->usage_min=umin&65535;f->usage_max=umax&65535;f->logical_min=g.lmin;
                    }
                }
                r->bits=end;r->bytes=(end+7)/8;
            }
            nu=0;have_min=have_max=false;
        }
    }
    if(depth || sp || !out->input_count)return false;
    if(out->uses_report_ids)for(unsigned i=0;i<out->input_count;i++)if(!out->inputs[i].report_id)return false;
    /* Only expose reports with decodable fields to the adapter. Unsupported
     * vendor-only reports must neither be subscribed nor consume key state. */
    unsigned kept=0;
    for(unsigned i=0;i<out->input_count;i++) {
        bool useful=false;
        for(unsigned j=0;j<out->field_count;j++)if(out->fields[j].report_id==out->inputs[i].report_id)useful=true;
        if(useful) {
            if(out->inputs[i].bits>512)return false;
            out->inputs[kept++]=out->inputs[i];
        }
    }
    out->input_count=kept;
    return kept!=0;
}
uint16_t hogp_usage_to_key(uint16_t usage_page, uint16_t usage)
{
    if (usage_page == 0x07) {
        switch (usage) {
        case 0x4F: return RBP_KEY_RIGHT;
        case 0x50: return RBP_KEY_LEFT;
        case 0x51: return RBP_KEY_DOWN;
        case 0x52: return RBP_KEY_UP;
        case 0x28: return RBP_KEY_OK;
        case 0x29: return RBP_KEY_BACK; /* Escape fallback */
        case 0x4A: return RBP_KEY_HOME;
        case 0x65: return RBP_KEY_MENU;
        case 0x66: return RBP_KEY_POWER;
        case 0x80: return RBP_KEY_VOLUME_UP;
        case 0x81: return RBP_KEY_VOLUME_DOWN;
        case 0x3E: return RBP_KEY_VOICE;
        case 0xF1: return RBP_KEY_BACK; /* Xiaomi extended back */
        case 0x35: return RBP_KEY_TV;
        default: return 0xFFFF;
        }
    }
    if (usage_page == 0x0C) {
        switch (usage) {
        case 0x030: return RBP_KEY_POWER;
        case 0x041: return RBP_KEY_OK;
        case 0x042: return RBP_KEY_UP;
        case 0x043: return RBP_KEY_DOWN;
        case 0x044: return RBP_KEY_LEFT;
        case 0x045: return RBP_KEY_RIGHT;
        case 0x0CF: return RBP_KEY_VOICE;
        case 0x0E9: return RBP_KEY_VOLUME_UP;
        case 0x0EA: return RBP_KEY_VOLUME_DOWN;
        case 0x040: return RBP_KEY_MENU;
        case 0x223: return RBP_KEY_HOME;  /* AC Home */
        case 0x224: return RBP_KEY_BACK;  /* AC Back */
        default: return 0xFFFF;
        }
    }
    return 0xFFFF;
}


bool hogp_decode_boot_keyboard(const uint8_t *data,size_t len,uint64_t *keys) {
    if(!data || !keys || len!=8 || data[1])return false;
    uint64_t result=0;
    for(unsigned i=2;i<8;i++) {
        if(data[i]>=1 && data[i]<=3)return false;
        uint16_t key=hogp_usage_to_key(7,data[i]);
        if(key!=0xffff && key && key<64)result|=(uint64_t)1<<key;
    }
    *keys=result;return true;
}

bool hogp_decode_report(const hogp_report_map_t *map,uint8_t id,const uint8_t *data,size_t len,uint64_t *keys) {
    if(!map || !data || !keys)return false;
    const hogp_input_report_t *r=NULL;
    for(unsigned i=0;i<map->input_count;i++)if(map->inputs[i].report_id==id)r=&map->inputs[i];
    if(!r || len!=r->bytes)return false;
    uint64_t result=0;
    for(unsigned i=0;i<map->field_count;i++) {
        const hogp_field_t*f=&map->fields[i];if(f->report_id!=id)continue;
        for(unsigned j=0;j<f->count;j++) {
            uint32_t value=0,bit=f->offset+j*f->size;
            for(unsigned k=0;k<f->size;k++)value|=((data[(bit+k)/8]>>((bit+k)%8))&1u)<<k;
            uint32_t usage;
            if(f->variable) {if(!value)continue;usage=f->usage_min;}
            else {
                if(value<f->logical_min || value-f->logical_min>(uint32_t)(f->usage_max-f->usage_min))return false;
                usage=f->usage_min+value-f->logical_min;
                if(f->page==7 && usage>=1 && usage<=3)return false; /* keyboard rollover/error */
            }
            uint16_t key=hogp_usage_to_key(f->page,(uint16_t)usage);
            if(key!=0xffff && key<64 && key)result|=(uint64_t)1<<key;
        }
    }
    *keys=result;return true;
}
