#pragma once
#include <stdbool.h>
typedef struct objc_object *id;
typedef struct objc_selector *SEL;
typedef struct objc_class *Class;
typedef struct objc_method *Method;
#define nil ((id)0)
#define Nil ((Class)0)
Class objc_getClass(const char *name);
SEL sel_registerName(const char *name);
Method class_getClassMethod(Class cls, SEL sel);
bool class_respondsToSelector(Class cls, SEL sel);
Class object_getClass(id obj);
