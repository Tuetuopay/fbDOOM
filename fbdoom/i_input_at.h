#ifndef __I_INPUT_AT__
#define __I_INPUT_AT__

#include "doomtype.h"

unsigned char I_TranslateKey(unsigned char key);
unsigned char I_GetTypedChar(unsigned char key);
void I_UpdateShiftStatus(boolean pressed, unsigned char key);

#endif
