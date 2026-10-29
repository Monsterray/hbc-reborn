#ifndef _MIIPLAY_H_
#define _MIIPLAY_H_

#include <gctypes.h>

// The Mii playground: HOME > HBC > Mii playground (miiplay.c). Runs until
// the user goes back; false when there is no Mii to play with.
bool show_mii_playground(void);

// The PC asks for it (HBCB p, `hbc.py buttons p`); main.c opens it from the
// app list. Asking clears it.
void miiplay_ask(void);
bool miiplay_asked(void);

// For HBCS: ,"playground":{...}, with the tab and page shown.
int miiplay_status_json(char *buf, int size);

#endif
