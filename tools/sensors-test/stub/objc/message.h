#pragma once
#include <objc/runtime.h>
extern void (*const objc_msgSend)(void);       /* a pointer here: gcc warns about calls through a cast function */
