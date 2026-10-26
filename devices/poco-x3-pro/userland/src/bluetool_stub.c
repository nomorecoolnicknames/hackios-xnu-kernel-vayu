/*
 * bluetool_stub: an answer where iOS's BlueTool would be, for a Bluetooth controller it has no scripts for.
 *
 * bluetoothd asks the Mach service com.apple.BlueTool to run its boot script before it powers the controller on,
 * and waits for the reply (from bluetoothd 16.5: FUN_1002722c8 and the loop FUN_1003a5f54): an XPC
 * dictionary whose int64 "result" is 0 for success (10 or 13: an unknown chipset, then a restart; anything else: asked
 * again for ever), with "firmware" (string, optional) and "manufacturer" (int64). Apple's BlueTool would send a
 * Broadcom script to vayu's Qualcomm controller; this sends nothing to the controller at all -- the kernel has
 * already loaded its firmware -- and answers success, naming that firmware and Qualcomm (29, its company identifier).
 * Every request is printed in full: what bluetoothd asks for is part of the measurement.
 *
 * Run by launchd as the job com.apple.BlueTool (MachServices com.apple.BlueTool), on demand, as mobile.
 */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include "xpc_min.h"            /* the iOS SDK has no <xpc/xpc.h> */

static void
answer(xpc_connection_t peer, xpc_object_t message)
{
	char *text = xpc_copy_description(message);
	xpc_object_t reply;

	fprintf(stderr, "bluetool_stub: request: %s\n", text ? text : "?");
	free(text);
	reply = xpc_dictionary_create_reply(message);
	if (reply == NULL) {
		fprintf(stderr, "bluetool_stub: the request wants no reply\n");
		return;
	}
	xpc_dictionary_set_int64(reply, "result", 0);
	xpc_dictionary_set_string(reply, "firmware", "crbtfw21");
	xpc_dictionary_set_int64(reply, "manufacturer", 29);
	xpc_connection_send_message(peer, reply);
	xpc_release(reply);
	fprintf(stderr, "bluetool_stub: answered result 0, firmware crbtfw21, manufacturer 29\n");
}

int
main(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", NULL,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	setvbuf(stderr, NULL, _IONBF, 0);
	fprintf(stderr, "bluetool_stub: listening on com.apple.BlueTool\n");
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != XPC_TYPE_CONNECTION) {
			char *text = xpc_copy_description(peer);

			fprintf(stderr, "bluetool_stub: listener event: %s\n", text ? text : "?");
			free(text);
			return;
		}
		fprintf(stderr, "bluetool_stub: a client connected (pid %d)\n", xpc_connection_get_pid(peer));
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			if (xpc_get_type(message) != XPC_TYPE_DICTIONARY) {
				char *text = xpc_copy_description(message);

				fprintf(stderr, "bluetool_stub: connection event: %s\n", text ? text : "?");
				free(text);
				return;
			}
			answer(peer, message);
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	dispatch_main();
}
