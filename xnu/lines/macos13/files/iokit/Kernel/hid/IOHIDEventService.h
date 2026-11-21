/*
 * A minimal IOHIDEventService: the part of IOHIDFamily's class that IOHIDEventServiceUserClient uses,
 * plus dispatchEvent() for the drivers of this kernel.
 *
 * Apple's IOHIDFamily kext is not part of xnu and this kernel loads no kexts. The user space's HID
 * event system (hidd and WindowServer on macOS 13, backboardd on iOS) finds IOService objects with
 * HIDServiceSupport = true, opens them with IOServiceOpen type 'esuc' and reads serialized
 * IOHIDEvents from the user client's shared queue. That user client, its queue and IOHIDEvent are
 * compiled here from Apple's IOHIDFamily-1915.120.2 (APSL 2.0; the HID family of macOS 13.4 22F66
 * and of the iOS 16 user space) with include paths and logging changed only, so that the bytes in
 * the queue are the ones the 13.4 event system parses. This class does not parse HID report
 * descriptors: there is no IOHIDDevice/IOHIDEventDriver below it, subclasses build the IOHIDEvents
 * that IOHIDEventDriver would have built (iokit/Kernel/VayuTouchPointer.cpp).
 *
 * Origin: the Scion kernel (github.com/Scion-Project/scion-kernel, iokit/Kernel/arm/hid/
 * IOHIDEventService.h and the base class in SweetHID.cpp, APSL 2.0), which did the same for its
 * phones; its copies of the IOHIDFamily files were changed to IOHIDFamily-2238's digitizer layout
 * and child framing for iOS 26, which is why the Apple files here are taken from 1915.120.2 instead.
 */
#ifndef _IOKIT_HID_IOHIDEVENTSERVICE_H
#define _IOKIT_HID_IOHIDEVENTSERVICE_H

#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include "IOHIDEventTypes.h"

class IOHIDEvent;

/* dispatchEvent options (IOHIDFamily IOHIDEventService.h) */
enum {
	kHIDDispatchOptionDeliveryNotificationForce         = (1 << 12),
	kHIDDispatchOptionDeliveryNotificationSuppress      = (1 << 13)
};

class IOHIDEventService : public IOService
{
	OSDeclareDefaultStructors(IOHIDEventService);

public:
	typedef void (*Action)(OSObject *target, OSObject *sender, void *context, OSObject *event, IOOptionBits options);

	virtual bool            start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void            free(void) APPLE_KEXT_OVERRIDE;

	virtual bool            open(IOService *client, IOOptionBits options, void *context, Action action);
	virtual void            close(IOService *forClient, IOOptionBits options = 0) APPLE_KEXT_OVERRIDE;

	virtual IOHIDEvent *    copyEvent(IOHIDEventType type, IOHIDEvent *matching = 0, IOOptionBits options = 0);
	virtual IOHIDEvent *    copyMatchingEvent(OSDictionary *matching);
	virtual IOReturn        setElementValue(UInt32 usagePage, UInt32 usage, UInt32 value);
	virtual IOReturn        setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;

	virtual UInt32          getPrimaryUsagePage();
	virtual UInt32          getPrimaryUsage();

	virtual IOReturn        newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    OSDictionary *properties, IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	/* the number of clients that have the service open now (the event system's user clients) */
	unsigned int            clientCount();

protected:
	virtual bool            handleOpen(IOService *client, IOOptionBits options, void *argument) APPLE_KEXT_OVERRIDE;
	virtual void            handleClose(IOService *client, IOOptionBits options) APPLE_KEXT_OVERRIDE;
	virtual bool            handleIsOpen(const IOService *client) const APPLE_KEXT_OVERRIDE;

	bool                    handleIsOpenLocked(const IOService *client) const;
	virtual void            dispatchEvent(IOHIDEvent *event, IOOptionBits options = 0);

	/* Published as HID properties by start(). */
	virtual OSString *      getTransport();
	virtual OSString *      getProduct();
	virtual OSString *      getManufacturer();
	virtual UInt32          getVendorID();
	virtual UInt32          getProductID();
	virtual UInt32          getReportInterval();

	IOWorkLoop *            _hidWorkLoop;
	IOCommandGate *         _hidGate;
	IOLock *                _clientLock;
	OSArray *               _clients;	/* IOHIDEventServiceClient per open client */
};

#endif /* _IOKIT_HID_IOHIDEVENTSERVICE_H */
