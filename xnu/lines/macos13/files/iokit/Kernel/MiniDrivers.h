/*
 * Stand-ins for Apple's closed kernel extensions: just enough of each for the stock iOS
 * user space to work on a machine without Apple hardware. The platform expert
 * (GenericARMPE) starts them once the registry is up.
 *
 *   MiniIOSurface.cpp              IOSurface: pixel buffers shared between processes
 *   MiniIOMobileFramebuffer.cpp    IOMobileFramebuffer: the display (the boot framebuffer)
 *   hakios.macgpu.cpp              AGXAccelerator: the GPU as macOS's Metal finds it (macos_gpu=1)
 *   MiniAppleKeyStore.cpp          AppleKeyStore: the keybag of a device without a passcode
 *   MiniIONetworkStack.cpp         IONetworkStack: what configd's interface namer waits for
 *   MiniAppleStockholm.cpp         AppleStockholmControl: what nfcd opens for the NFC controller (vayu_nfc_ios=1)
 *   GenericARMPE.cpp               IOHIDSystem: what macOS's WindowServer opens first (macos_abi=1)
 *   GenericARMPE.cpp               EndpointSecurityDriver: what macOS's launchd boot task waits for (macos_es=1)
 *   HakiosAPFSMedia.cpp            IOMedia, AppleAPFSContainer, AppleAPFSVolume: the disks and APFS containers as
 *                                  macOS's apfs_boot_util finds them (macos_abi=1; bsd/miscfs/apfs/apfs_media.c)
 *   VayuTouchPointer.cpp           HID event services for macOS: the touch screen as a pointer, the buttons (macos_hid=1)
 */
#ifndef _IOKIT_MINIDRIVERS_H
#define _IOKIT_MINIDRIVERS_H

#include <IOKit/IOService.h>

void MiniIOSurfaceStart(IOService * provider);
void MiniIOMobileFramebufferStart(IOService * provider);
void HakiosMacAcceleratorStart(IOService * provider);
void MiniAppleKeyStoreStart(void);
void MiniIONetworkStackStart(void);
void MiniAppleStockholmStart(void);
void MiniIOHIDSystemStart(IOService * provider);
void MiniEndpointSecurityStart(IOService * provider);
extern "C" void apfs_media_iokit_ready(void);     /* bsd/miscfs/apfs/apfs_media.c */

extern "C" uint32_t hakios_macos_abi;      /* bsd/kern/kern_exec.c: boot-arg macos_abi=1 */

/*
 * The accelerator's classes, defined in MiniIOMobileFramebuffer.cpp: IOAcceleratorES, the class iOS's Metal
 * matches, and IOAccelerator, the class macOS's matches. hakios.macgpu.cpp's AGXAccelerator is an IOAccelerator
 * too: one class of that name in the kernel, or the two files' metaclasses collide at link time.
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

class IOAccelerator : public IOAcceleratorES
{
	OSDeclareDefaultStructors(IOAccelerator);
};
void VayuTouchPointerStart(void);

#endif /* _IOKIT_MINIDRIVERS_H */
