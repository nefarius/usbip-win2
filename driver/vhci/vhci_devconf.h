#pragma once

#include <ntdef.h> 
#include <wdm.h>
#include <usbdi.h>

#include <stdbool.h>

#include "usbip_proto.h" 

extern const USBD_PIPE_HANDLE EP0;

NTSTATUS setup_config(struct _URB_SELECT_CONFIGURATION *cfg, enum usb_device_speed speed);
NTSTATUS setup_intf(USBD_INTERFACE_INFORMATION *intf_info, enum usb_device_speed speed, USB_CONFIGURATION_DESCRIPTOR *cfgd);

enum { 
	SELECT_CONFIGURATION_STR_BUFSZ = 1024, 
	SELECT_INTERFACE_STR_BUFSZ = SELECT_CONFIGURATION_STR_BUFSZ 
};

const char *select_configuration_str(char *buf, size_t len, const struct _URB_SELECT_CONFIGURATION *cfg);
const char *select_interface_str(char *buf, size_t len, const struct _URB_SELECT_INTERFACE *iface);

/*
 * @return bEndpointAddress of endpoint descriptor 
 */
__inline UCHAR get_endpoint_address(USBD_PIPE_HANDLE handle)
{
	UCHAR *v = (UCHAR*)&handle;
	return v[0];
}

__inline UCHAR get_endpoint_interval(USBD_PIPE_HANDLE handle)
{
	UCHAR *v = (UCHAR*)&handle;
	return v[1];
}

__inline USBD_PIPE_TYPE get_endpoint_type(USBD_PIPE_HANDLE handle)
{
	UCHAR *v = (UCHAR*)&handle;
	return v[2];
}

__inline UCHAR get_endpoint_number(USBD_PIPE_HANDLE handle)
{
	UCHAR addr = get_endpoint_address(handle);
	return addr & USB_ENDPOINT_ADDRESS_MASK;
}

/*
 * EP0 is bidirectional. 
 */
__inline bool is_endpoint_direction_in(USBD_PIPE_HANDLE handle)
{
	NT_ASSERT(handle);
	UCHAR addr = get_endpoint_address(handle);
	return USB_ENDPOINT_DIRECTION_IN(addr);
}

/*
* EP0 is bidirectional. 
*/
__inline bool is_endpoint_direction_out(USBD_PIPE_HANDLE handle)
{
	NT_ASSERT(handle);
	UCHAR addr = get_endpoint_address(handle);
	return USB_ENDPOINT_DIRECTION_OUT(addr);
}
