	.set noreorder

#include "pspstub.s"

	STUB_START "pspRelaunch",0x40090000,0x00010005
	STUB_FUNC  0x2C978466,pspRelaunchSelf
	STUB_END
