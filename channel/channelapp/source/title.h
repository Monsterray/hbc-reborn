#ifndef _TITLE_H_
#define _TITLE_H_

#include <gctypes.h>
#include "../config.h"

const char *title_get_path(void);
u64 title_get_id(void);
void title_init(void);
bool is_vwii(void);

#endif

