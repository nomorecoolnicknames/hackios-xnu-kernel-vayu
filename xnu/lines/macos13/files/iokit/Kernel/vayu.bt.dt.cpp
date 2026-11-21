/*
 * The "bluetooth" service bluetoothd looks for: the device-tree entry iOS reads the local address
 * and the HCI transport from.
 *
 * iOS 16.5/17 bluetoothd (usr/sbin/bluetoothd, FindLocalMacAddress and the port check before it,
 * disassembly 2026-11-02) does, in this order:
 *
 *   IOServiceNameMatching("marconi-bt") -> IOServiceGetMatchingService()
 *   IORegistryEntrySearchCFProperty(service, "IODeviceTree", <key>, kCFAllocatorDefault,
 *                                   kIORegistryIterateRecursively)
 *
 * and repeats the same with "bluetooth" when "marconi-bt" does not match. The keys are
 * "local-mac-address" (6 raw bytes: CFDataGetBytes of 6), "transport-encoding" (4 bytes, 1 = H4) and
 * "transport-speed" (4 bytes). Without the service the daemon falls back to BT_DEVICE_ADDRESS and the
 * com.apple.BTServer plist, and with neither of them it logs "Not a valid MAC address (all 00s)" and
 * exits with "No bluetooth on this device" (nightly run of 2026-11-01, another kernel).
 *
 * The registry search reads the property of the matched entry itself before it walks any plane, so an
 * IOService with these properties is enough; "IODeviceTree" is only the plane the caller names.
 * There is no driver for the class and no user client: it carries properties, as hakios.wlan.cpp's
 * IO80211Controller does for wifid. Both names are published, one instance each, and
 * vayu_bt_dt_publish() may be called again to update the address (early from the NVM file, then from
 * the controller's own Read_BD_ADDR).
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>

extern "C" bool vayu_bt_dt_publish(const uint8_t * mac, uint32_t transport, uint32_t speed);

#define VAYU_BT_DT_NAMES        2
#define VAYU_BT_DT_TRANSPORT_H4 1               /* bluetoothd's encoding: H4 over the POSIX driver */

class VayuBluetoothNub : public IOService
{
	OSDeclareDefaultStructors(VayuBluetoothNub);
};

OSDefineMetaClassAndStructors(VayuBluetoothNub, IOService);

static VayuBluetoothNub * g_vayu_bt_nub[VAYU_BT_DT_NAMES];
static const char * const g_vayu_bt_name[VAYU_BT_DT_NAMES] = { "marconi-bt", "bluetooth" };

static void
vayu_bt_set_data(IOService * service, const char * key, const void * bytes, unsigned int length)
{
	OSData * data = OSData::withBytes(bytes, length);

	if (data != NULL) {
		service->setProperty(key, data);
		data->release();
	}
}

/* One property carrier under the platform, registered so that IOServiceGetMatchingService() sees it. */
static VayuBluetoothNub *
vayu_bt_dt_make(IOService * platform, const char * name, const uint8_t * mac, uint32_t transport,
    uint32_t speed)
{
	VayuBluetoothNub * nub = OSTypeAlloc(VayuBluetoothNub);

	if (nub == NULL || !nub->init()) {
		OSSafeReleaseNULL(nub);
		return NULL;
	}
	vayu_bt_set_data(nub, "local-mac-address", mac, 6);
	vayu_bt_set_data(nub, "transport-encoding", &transport, sizeof(transport));
	vayu_bt_set_data(nub, "transport-speed", &speed, sizeof(speed));
	nub->setProperty("built-in", true);

	if (!nub->attach(platform)) {
		nub->release();
		return NULL;
	}
	nub->setName(name);
	if (!nub->start(platform)) {
		nub->detach(platform);
		nub->release();
		return NULL;
	}
	nub->registerService();
	return nub;
}

extern "C" bool
vayu_bt_dt_publish(const uint8_t * mac, uint32_t transport, uint32_t speed)
{
	IOService * platform;
	bool published = false;

	if (mac == NULL) {
		return false;
	}
	if (g_vayu_bt_nub[0] != NULL) {
		/* the address may arrive after the node: NVM first, the controller's Read_BD_ADDR second */
		for (unsigned int i = 0; i < VAYU_BT_DT_NAMES; i++) {
			if (g_vayu_bt_nub[i] != NULL) {
				vayu_bt_set_data(g_vayu_bt_nub[i], "local-mac-address", mac, 6);
				published = true;
			}
		}
		return published;
	}

	platform = IOService::getPlatform();
	if (platform == NULL) {
		IOLog("vayu-bt: S10 dt: there is no platform expert\n");
		return false;
	}
	for (unsigned int i = 0; i < VAYU_BT_DT_NAMES; i++) {
		g_vayu_bt_nub[i] = vayu_bt_dt_make(platform, g_vayu_bt_name[i], mac, transport, speed);
		if (g_vayu_bt_nub[i] != NULL) {
			published = true;
		} else {
			IOLog("vayu-bt: S10 dt: \"%s\" was not published\n", g_vayu_bt_name[i]);
		}
	}
	return published;
}
