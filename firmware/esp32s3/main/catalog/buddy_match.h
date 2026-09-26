#pragma once
#include <stdbool.h>
#include <string.h>
/* Scan hints are admission evidence, never a ranking that chooses a model. */
static inline bool buddy_hint_matches(const char *exact,const char *prefix,int required_company,const char *name,int company,bool ignore_company) {
 return name && *name && (ignore_company || required_company<0 || required_company==company) &&
   ((exact && *exact && !strcmp(exact,name)) || (prefix && *prefix && !strncmp(prefix,name,strlen(prefix))));
}
static inline int buddy_match_merge(int found,int model) {return found==-1 || found==model ? model : -2;}
