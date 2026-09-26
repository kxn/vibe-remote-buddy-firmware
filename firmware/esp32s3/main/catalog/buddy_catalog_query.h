#pragma once
#include "buddy_catalog.h"
#include "buddy_models.h"
bool buddy_catalog_model(const buddy_catalog_t *,uint32_t,buddy_model_t *);
int buddy_catalog_find(const buddy_catalog_t *,const char *id);
int buddy_catalog_match_name(const buddy_catalog_t *,const char *name,int company,bool ignore_company);
/* Candidate-stage Map verification. Additional fingerprint requirements are
 * checked at binding commit after services and report references are known. */
int buddy_catalog_match_map(const buddy_catalog_t *,const uint8_t *,size_t,int selected);
typedef bool (*buddy_catalog_evidence_fn)(void *,const cJSON *required);
int buddy_catalog_identify(const buddy_catalog_t *,const uint8_t *,size_t,int,buddy_catalog_evidence_fn,void *);

bool buddy_catalog_validate_lookups(const buddy_catalog_t *);
