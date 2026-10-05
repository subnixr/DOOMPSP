#include <pspsdk.h>
#include <pspkernel.h>
#include <psploadexec_kernel.h>
#include <systemctrl.h>

#include <string.h>

PSP_MODULE_INFO("pspRelaunch_Module", 0x1006, 1, 0);

// Reboot into the given executable. The caller passes the apitype it was
// started with (memory stick or internal storage). Only returns on failure.
int pspRelaunchSelf(int apitype, const char *path)
{
	int k1 = pspSdkSetK1(0);
	struct SceKernelLoadExecVSHParam param;
	int res;

	memset(&param, 0, sizeof(param));
	param.size = sizeof(param);
	param.args = strlen(path) + 1;
	param.argp = (void *)path;
	param.key = "game";

	res = sctrlKernelLoadExecVSHWithApitype(apitype, path, &param);

	pspSdkSetK1(k1);
	return res;
}

int module_start(SceSize args, void *argp)
{
	return 0;
}

int module_stop(SceSize args, void *argp)
{
	return 0;
}
