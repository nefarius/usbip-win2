#pragma once

#include <guiddef.h>

#ifdef _KERNEL_MODE
  #include <ntddk.h>
#else
  #include <windows.h>
  #include <winioctl.h>
#endif

#include <usbspec.h>

#include "ch9.h"
#include "consts.h"
#include "proto.h"

namespace usbip::vhci
{

enum hci_version { HCI_USB2, HCI_USB3 };

enum { 
        USB2_PORTS = 30,
        USB3_PORTS = USB2_PORTS,
        TOTAL_PORTS = USB2_PORTS + USB3_PORTS
};

constexpr auto is_valid_port(int port)
{
        return port > 0 && port <= TOTAL_PORTS;
}

constexpr auto get_num_ports(hci_version version)
{
        return version == HCI_USB2 ? USB2_PORTS : USB3_PORTS;
}

constexpr auto get_hci_version(int port) // [1..TOTAL_PORTS]
{
        return port <= USB2_PORTS ? HCI_USB2 : HCI_USB3;
}
static_assert(get_hci_version(1) == (USB2_PORTS ? HCI_USB2 : HCI_USB3));
static_assert(get_hci_version(USB2_PORTS) == HCI_USB2);
static_assert(get_hci_version(USB2_PORTS + 1) == HCI_USB3);
static_assert(get_hci_version(TOTAL_PORTS) == (USB3_PORTS ? HCI_USB3 : HCI_USB2));

constexpr auto make_port(hci_version version, int number) // [1..get_num_ports(version)]
{
        return int(version)*USB2_PORTS + number;
}
static_assert(make_port(HCI_USB2, 0) == 0);
static_assert(make_port(HCI_USB2, 1) == 1);
static_assert(make_port(HCI_USB2, USB2_PORTS) == USB2_PORTS);
static_assert(make_port(HCI_USB3, 1) == USB2_PORTS + 1);
static_assert(make_port(HCI_USB3, USB3_PORTS) == TOTAL_PORTS);

static_assert(get_hci_version(make_port(HCI_USB2, 1)) == (USB2_PORTS ? HCI_USB2 : HCI_USB3));
static_assert(get_hci_version(make_port(HCI_USB2, USB2_PORTS)) == HCI_USB2);
static_assert(get_hci_version(make_port(HCI_USB2, USB2_PORTS + 1)) == HCI_USB3);
static_assert(get_hci_version(make_port(HCI_USB3, 1)) == HCI_USB3);
static_assert(get_hci_version(make_port(HCI_USB3, USB3_PORTS)) == (USB3_PORTS ? HCI_USB3 : HCI_USB2));


DEFINE_GUID(GUID_DEVINTERFACE_USB_HOST_CONTROLLER,
        0xB4030C06, 0xDC5F, 0x4FCC, 0x87, 0xEB, 0xE5, 0x51, 0x5A, 0x09, 0x35, 0xC0);

constexpr auto IOCTL(int idx)
{
        return CTL_CODE(FILE_DEVICE_BUS_EXTENDER, idx, METHOD_BUFFERED, FILE_READ_DATA);
}

enum {
        IOCTL_PLUGIN_HARDWARE      = IOCTL(0),
        IOCTL_PLUGOUT_HARDWARE     = IOCTL(1),
        IOCTL_GET_IMPORTED_DEVICES = IOCTL(2),
};

struct ioctl_plugin
{
        int port; // OUT, must be the first member; [1..TOTAL_PORTS] in (port & 0xFFFF) or see make_error()
        char busid[USBIP_BUS_ID_SIZE];
        char service[32]; // NI_MAXSERV
        char host[1025];  // NI_MAXHOST in ws2def.h
        char serial[255];
};

struct ioctl_imported_dev : ioctl_plugin
{
        usbip_device_status status;
        unsigned short vendor;
        unsigned short product;
        UINT32 devid;
        usb_device_speed speed;
};

struct ioctl_plugout
{
        int port; // [1..TOTAL_PORTS] or all ports if <= 0
};

} // namespace usbip::vhci
