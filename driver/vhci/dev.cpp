#include "dev.h"
#include "trace.h"
#include "dev.tmh"
#include "pageable.h"
#include "vhci.h"
#include "irp.h"

#include <limits.h>
#include <wsk.h>
#include <wdmsec.h>
#include <initguid.h> // required for GUID definitions

DEFINE_GUID(GUID_SD_USBIP_VHCI,
	0x9d3039dd, 0xcca5, 0x4b4d, 0xb3, 0x3d, 0xe2, 0xdd, 0xc8, 0xa8, 0xc5, 0x2f);

void *GetDeviceProperty(DEVICE_OBJECT *obj, DEVICE_REGISTRY_PROPERTY prop, NTSTATUS &error, ULONG &ResultLength)
{
	ResultLength = 256;
	auto alloc = [] (auto len) { return ExAllocatePool2(POOL_FLAG_PAGED|POOL_FLAG_UNINITIALIZED, len, USBIP_VHCI_POOL_TAG); };

	for (auto buf = alloc(ResultLength); buf; ) {
		
		error = IoGetDeviceProperty(obj, prop, ResultLength, buf, &ResultLength);
		
		switch (error) {
		case STATUS_SUCCESS:
			return buf;
		case STATUS_BUFFER_TOO_SMALL:
			ExFreePoolWithTag(buf, USBIP_VHCI_POOL_TAG);
			buf = alloc(ResultLength);
			break;
		default:
			ExFreePoolWithTag(buf, USBIP_VHCI_POOL_TAG);
			return nullptr;
		}
	}

	error = USBD_STATUS_INSUFFICIENT_RESOURCES;
	return nullptr;
}

PAGEABLE PDEVICE_OBJECT vdev_create(DRIVER_OBJECT *drvobj, vdev_type_t type)
{
	PAGED_CODE();

        const ULONG ext_sizes[] = 
        {
                sizeof(root_dev_t),
                sizeof(cpdo_dev_t),
                sizeof(vhci_dev_t),
                sizeof(hpdo_dev_t),
                sizeof(vhub_dev_t),
                sizeof(vpdo_dev_t)
        };

        static_assert(ARRAYSIZE(ext_sizes) == VDEV_SIZE);

	DEVICE_OBJECT *devobj{};
	auto extsize = ext_sizes[type];
	NTSTATUS status{};

	switch (type) {
	case VDEV_CPDO:
	case VDEV_HPDO:
	case VDEV_VPDO:
		status = IoCreateDeviceSecure(drvobj, extsize, nullptr,
				FILE_DEVICE_BUS_EXTENDER, FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN,
				FALSE, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX_RES_RWX, // allow normal users to access the devices
				&GUID_SD_USBIP_VHCI, &devobj);
		break;
	default:
		status = IoCreateDevice(drvobj, extsize, nullptr,
					FILE_DEVICE_BUS_EXTENDER, FILE_DEVICE_SECURE_OPEN, TRUE, &devobj);
	}

	if (!NT_SUCCESS(status)) {
		Trace(TRACE_LEVEL_ERROR, "Failed to create vdev(%!vdev_type_t!): %!STATUS!", type, status);
		return nullptr;
	}

	auto vdev = to_vdev(devobj);

        vdev->Self = devobj;
        vdev->type = type;

        NT_ASSERT(vdev->PnPState == pnp_state::NotStarted);
        NT_ASSERT(vdev->PreviousPnPState == pnp_state::NotStarted);

        vdev->SystemPowerState = PowerSystemWorking;
        NT_ASSERT(vdev->DevicePowerState == PowerDeviceUnspecified);

	devobj->Flags |= DO_POWER_PAGABLE | DO_BUFFERED_IO;

	return devobj;
}

vhub_dev_t *vhub_from_vhci(vhci_dev_t *vhci)
{	
	NT_ASSERT(vhci);
	auto child_pdo = vhci->child_pdo;
	return child_pdo ? reinterpret_cast<vhub_dev_t*>(child_pdo->fdo) : nullptr;
}

cpdo_dev_t *to_cpdo_or_null(DEVICE_OBJECT *devobj)
{
	auto vdev = to_vdev(devobj);
	return vdev->type == VDEV_CPDO ? static_cast<cpdo_dev_t*>(vdev) : nullptr;
}

vhci_dev_t *to_vhci_or_null(DEVICE_OBJECT *devobj)
{
	auto vdev = to_vdev(devobj);
	return vdev->type == VDEV_VHCI ? static_cast<vhci_dev_t*>(vdev) : nullptr;
}

hpdo_dev_t *to_hpdo_or_null(DEVICE_OBJECT *devobj)
{
	auto vdev = to_vdev(devobj);
	return vdev->type == VDEV_HPDO ? static_cast<hpdo_dev_t*>(vdev) : nullptr;
}

vhub_dev_t *to_vhub_or_null(DEVICE_OBJECT *devobj)
{
	auto vdev = to_vdev(devobj);
	return vdev->type == VDEV_VHUB ? static_cast<vhub_dev_t*>(vdev) : nullptr;
}

vpdo_dev_t *to_vpdo_or_null(DEVICE_OBJECT *devobj)
{
	auto vdev = to_vdev(devobj);
	return vdev->type == VDEV_VPDO ? static_cast<vpdo_dev_t*>(vdev) : nullptr;
}

/*
 * @see is_valid_seqnum
 */
seqnum_t next_seqnum(vpdo_dev_t &vpdo, bool dir_in)
{
	static_assert(!USBIP_DIR_OUT);
	static_assert(USBIP_DIR_IN);

	static_assert(sizeof(vpdo.seqnum) == sizeof(LONG));

	while (true) {
		if (seqnum_t num = InterlockedIncrement(reinterpret_cast<LONG*>(&vpdo.seqnum)) << 1) {
			return num |= seqnum_t(dir_in);
		}
	}
}

/*
 * Zero string index means absense of a descriptor.
 */
PCWSTR get_string_descr_str(const vpdo_dev_t &vpdo, UCHAR index)
{
	if (index && index < ARRAYSIZE(vpdo.strings)) {
		if (auto d = vpdo.strings[index]) {
			return d->bString;
		}
	}

	return nullptr;
}

/*
 * @return true if list was not empty
 */
bool complete_enqueue(_Inout_ vpdo_dev_t &vpdo, _In_ IRP *irp)
{
	return ExInterlockedInsertTailList(&vpdo.complete, list_entry(irp), &vpdo.complete_lock);
}

IRP *complete_dequeue(_Inout_ vpdo_dev_t &vpdo)
{
	IRP *irp{};

	if (auto entry = ExInterlockedRemoveHeadList(&vpdo.complete, &vpdo.complete_lock)) {
		InitializeListHead(entry);
		irp = get_irp(entry);  
	}

	return irp;
}
