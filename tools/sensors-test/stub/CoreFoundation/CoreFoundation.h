/* Host stand-in for the little of CoreFoundation that sensors.c and motion.c use: enough to compile
 * and to run the paths the tests drive (the definitions are in ../../fake.h). */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef unsigned char Boolean;
typedef long CFIndex;
typedef unsigned long CFTypeID;
typedef const void *CFTypeRef;
typedef const struct __CFString *CFStringRef;
typedef const struct __CFNumber *CFNumberRef;
typedef const struct __CFBoolean *CFBooleanRef;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFArray *CFArrayRef;
typedef const struct __CFAllocator *CFAllocatorRef;
typedef struct { int unused; } CFDictionaryKeyCallBacks, CFDictionaryValueCallBacks, CFArrayCallBacks;
typedef int CFNumberType;
typedef unsigned int CFStringEncoding;
enum { kCFNumberIntType = 9, kCFNumberDoubleType = 13 };
enum { kCFStringEncodingUTF8 = 0x08000100 };
#define CFSTR(s) ((CFStringRef)(s))

extern const CFBooleanRef kCFBooleanTrue, kCFBooleanFalse;
extern const CFDictionaryKeyCallBacks kCFTypeDictionaryKeyCallBacks;
extern const CFDictionaryValueCallBacks kCFTypeDictionaryValueCallBacks;
extern const CFArrayCallBacks kCFTypeArrayCallBacks;

CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef, CFIndex, const CFDictionaryKeyCallBacks *,
    const CFDictionaryValueCallBacks *);
void CFDictionarySetValue(CFMutableDictionaryRef, const void *, const void *);
const void *CFDictionaryGetValue(CFDictionaryRef, const void *);
CFIndex CFDictionaryGetCount(CFDictionaryRef);
void CFDictionaryGetKeysAndValues(CFDictionaryRef, const void **, const void **);
CFArrayRef CFArrayCreate(CFAllocatorRef, const void **, CFIndex, const CFArrayCallBacks *);
CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char *, CFStringEncoding);
Boolean CFStringGetCString(CFStringRef, char *, CFIndex, CFStringEncoding);
CFNumberRef CFNumberCreate(CFAllocatorRef, CFNumberType, const void *);
Boolean CFNumberGetValue(CFNumberRef, CFNumberType, void *);
CFTypeRef CFRetain(CFTypeRef);
void CFRelease(CFTypeRef);
Boolean CFEqual(CFTypeRef, CFTypeRef);
CFTypeID CFGetTypeID(CFTypeRef);
CFTypeID CFBooleanGetTypeID(void);
CFTypeID CFNumberGetTypeID(void);
Boolean CFBooleanGetValue(CFBooleanRef);
CFStringRef CFCopyDescription(CFTypeRef);
