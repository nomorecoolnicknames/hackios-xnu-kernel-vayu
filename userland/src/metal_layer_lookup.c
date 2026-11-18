/*
 * metal_layer_lookup.dylib: what lets `metal_test layer` (metal_layer.m, task 10) reach the window server from sweetd.
 *
 * backboardd's services (com.apple.CARenderServer, the display and HID services) belong to a job of user mobile, so
 * launchd keeps them in the user/501 domain. A process started by sweetd, a system job, looks names up in the system
 * domain, and launchd says "failed lookup ... (system) ... No such process" (FACT, 2026-11-07, phone log): it picks
 * the domain by the job a process descends from, not by its uid, and does not give out SpringBoard's bootstrap port.
 * It does answer bootstrap_look_up_per_user(…, 501, …) from root. So a lookup the system domain refuses is asked again
 * of user 501's here, interposed under every image's bootstrap_look_up, _up2 and _up3 (QuartzCore's is _up3) (dyld honours __interpose
 * in an inserted library, not in the main executable). metal_test sets DYLD_INSERT_LIBRARIES to this library beside
 * itself and runs again.
 *
 * From another HakiOS port (MIT, as this file): telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (its Metal tasks 8-11).
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);
extern kern_return_t bootstrap_look_up2(mach_port_t, const char *, mach_port_t *, pid_t, uint64_t);
extern kern_return_t bootstrap_look_up3(mach_port_t, const char *, mach_port_t *, pid_t, const unsigned char *, uint64_t);

static int answered;

static kern_return_t
retry(mach_port_t bp, const char *name, mach_port_t *sp, kern_return_t kr)
{
	typedef kern_return_t (*per_user_t)(mach_port_t, const char *, uid_t, mach_port_t *);
	static per_user_t per_user;

	if (kr == KERN_SUCCESS || getuid() != 0) {
		return kr;
	}
	if (per_user == NULL) {
		per_user = (per_user_t)dlsym(RTLD_DEFAULT, "bootstrap_look_up_per_user");
	}
	if (per_user != NULL && per_user(bp, name, 501, sp) == KERN_SUCCESS) {
		if (answered++ < 40) {
			printf("metal_layer:   (lookup of %s answered by user 501's domain)\n", name);
		}
		return KERN_SUCCESS;
	}
	return kr;
}

static kern_return_t
look_up(mach_port_t bp, const char *name, mach_port_t *sp)
{
	return retry(bp, name, sp, bootstrap_look_up(bp, name, sp));
}

static kern_return_t
look_up2(mach_port_t bp, const char *name, mach_port_t *sp, pid_t pid, uint64_t flags)
{
	return retry(bp, name, sp, bootstrap_look_up2(bp, name, sp, pid, flags));
}

static kern_return_t
look_up3(mach_port_t bp, const char *name, mach_port_t *sp, pid_t pid, const unsigned char *instance, uint64_t flags)
{
	return retry(bp, name, sp, bootstrap_look_up3(bp, name, sp, pid, instance, flags));
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement, *replacee;
} interpose[] = {
	{ (const void *)look_up, (const void *)bootstrap_look_up },
	{ (const void *)look_up2, (const void *)bootstrap_look_up2 },
	{ (const void *)look_up3, (const void *)bootstrap_look_up3 },
};
