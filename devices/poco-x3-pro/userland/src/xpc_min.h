/*
 * The few declarations of libxpc (part of libSystem) that bluetool_stub.c needs. The iOS SDK ships no <xpc/xpc.h>;
 * these are the public C interface as Apple documents it for macOS, where the same library has the same symbols.
 */
#ifndef XPC_MIN_H
#define XPC_MIN_H
#include <dispatch/dispatch.h>
#include <stdint.h>
#include <sys/types.h>

typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef const struct _xpc_type_s *xpc_type_t;
typedef void (^xpc_handler_t)(xpc_object_t object);

extern const struct _xpc_type_s _xpc_type_connection;
extern const struct _xpc_type_s _xpc_type_dictionary;
#define XPC_TYPE_CONNECTION (&_xpc_type_connection)
#define XPC_TYPE_DICTIONARY (&_xpc_type_dictionary)
#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)

xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags);
void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
void xpc_connection_resume(xpc_connection_t connection);
void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
pid_t xpc_connection_get_pid(xpc_connection_t connection);
xpc_type_t xpc_get_type(xpc_object_t object);
char *xpc_copy_description(xpc_object_t object);
xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
void xpc_dictionary_set_int64(xpc_object_t dictionary, const char *key, int64_t value);
void xpc_dictionary_set_string(xpc_object_t dictionary, const char *key, const char *string);
void xpc_release(xpc_object_t object);
#endif
