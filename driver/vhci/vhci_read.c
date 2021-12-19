#include "vhci_read.h"
#include "dbgcommon.h"
#include "trace.h"
#include "vhci_read.tmh"

#include "vhci_irp.h"
#include "vhci_proto.h"
#include "vhci_internal_ioctl.h"
#include "usbd_helper.h"
#include "pdu.h"
#include "ch9.h"
#include "ch11.h"

#define TRANSFERRED(irp) ((irp)->IoStatus.Information)

static PAGEABLE __inline void *get_irp_buffer(const IRP *read_irp)
{
	return read_irp->AssociatedIrp.SystemBuffer;
}

static PAGEABLE ULONG get_irp_buffer_size(const IRP *read_irp)
{
	IO_STACK_LOCATION *irpstack = IoGetCurrentIrpStackLocation((IRP*)read_irp);
	return irpstack->Parameters.Read.Length;
}

static PAGEABLE void *try_get_irp_buffer(const IRP *irp, size_t min_size)
{
	NT_ASSERT(!TRANSFERRED(irp));
	ULONG sz = get_irp_buffer_size(irp);
	return sz >= min_size ? get_irp_buffer(irp) : NULL;
}

static PAGEABLE struct usbip_header *get_irp_buffer_hdr(const IRP *irp, ULONG irp_buf_sz)
{
	NT_ASSERT(!TRANSFERRED(irp));
	return irp_buf_sz >= sizeof(struct usbip_header) ? get_irp_buffer(irp) : NULL;
}

static PAGEABLE const void *get_buf(void *buf, MDL *bufMDL)
{
	if (buf) {
		return buf;
	}

	if (!bufMDL) {
		TraceError(TRACE_READ, "TransferBuffer and TransferBufferMDL are NULL");
		return NULL;
	}

	buf = MmGetSystemAddressForMdlSafe(bufMDL, LowPagePriority | MdlMappingNoExecute | MdlMappingNoWrite);
	if (!buf) {
		TraceError(TRACE_READ, "MmGetSystemAddressForMdlSafe error");
	}

	return buf;
}

/*
* USBD_ISO_PACKET_DESCRIPTOR.Length is not used (zero) for USB_DIR_OUT transfer.
*/
static PAGEABLE NTSTATUS do_copy_payload(void *dst_buf, const struct _URB_ISOCH_TRANSFER *r, ULONG *transferred)
{
	NT_ASSERT(dst_buf);

	*transferred = 0;
	void *buf_a = r->Hdr.Function == URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL ? NULL : r->TransferBuffer;

	const void *src_buf = get_buf(buf_a, r->TransferBufferMDL);
	if (!src_buf) {
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	bool dir_out = IsTransferDirectionOut(r->TransferFlags);
	ULONG buf_len = dir_out ? r->TransferBufferLength : 0;

	RtlCopyMemory(dst_buf, src_buf, buf_len);
	*transferred += buf_len;

	struct usbip_iso_packet_descriptor *dsc = (void*)((char*)dst_buf + buf_len);
	ULONG sum = 0;

	for (ULONG i = 0; i < r->NumberOfPackets; ++dsc) {

		ULONG offset = r->IsoPacket[i].Offset;
		ULONG next_offset = ++i < r->NumberOfPackets ? r->IsoPacket[i].Offset : r->TransferBufferLength;

		if (next_offset >= offset && next_offset <= r->TransferBufferLength) {
			dsc->offset = offset;
			dsc->length = next_offset - offset;
			dsc->actual_length = 0;
			dsc->status = 0;
			sum += dsc->length;
		} else {
			TraceError(TRACE_READ, "[%lu] next_offset(%lu) >= offset(%lu) && next_offset <= r->TransferBufferLength(%lu)",
						i, next_offset, offset, r->TransferBufferLength);

			return STATUS_INVALID_PARAMETER;
		}
	}

	*transferred += r->NumberOfPackets*sizeof(*dsc);

	NT_ASSERT(sum == r->TransferBufferLength);
	return STATUS_SUCCESS;
}

static PAGEABLE ULONG get_payload_size(const struct _URB_ISOCH_TRANSFER *r)
{
	ULONG len = r->NumberOfPackets*sizeof(struct usbip_iso_packet_descriptor);

	if (IsTransferDirectionOut(r->TransferFlags)) {
		len += r->TransferBufferLength;
	}

	return len;
}

static PAGEABLE NTSTATUS copy_transfer_buffer(void *dst, const URB *urb, IRP *irp)
{
	NT_ASSERT(dst);

	bool mdl = urb->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL;
	NT_ASSERT(urb->UrbHeader.Function != URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL);

	const struct _URB_CONTROL_TRANSFER *r = &urb->UrbControlTransfer; // any struct with Transfer* members can be used

	const void *buf = get_buf(mdl ? NULL : r->TransferBuffer, r->TransferBufferMDL);
	if (buf) {
		RtlCopyMemory(dst, buf, r->TransferBufferLength);
		TRANSFERRED(irp) += r->TransferBufferLength;
	}

	return buf ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static PAGEABLE NTSTATUS copy_payload(void *dst, IRP *irp, const struct _URB_ISOCH_TRANSFER *r, ULONG expected)
{
	ULONG transferred = 0;
	NTSTATUS err = do_copy_payload(dst, r, &transferred);

	if (!err) {
		NT_ASSERT(transferred == expected);
		TRANSFERRED(irp) += transferred;
	}

	return err;
}

/*
* Copy usbip payload to read buffer, usbip_header was handled by previous IRP.
* Userspace app reads usbip header (previous IRP), calculates usbip payload size, reads usbip payload (this IRP).
*/
static PAGEABLE NTSTATUS transfer_partial(IRP *irp, URB *urb)
{
	const struct _URB_CONTROL_TRANSFER *r = &urb->UrbControlTransfer; // any struct with Transfer* members can be used
	void *dst = try_get_irp_buffer(irp, r->TransferBufferLength);

	return dst ? copy_transfer_buffer(dst, urb, irp) : STATUS_BUFFER_TOO_SMALL;
}

static PAGEABLE NTSTATUS urb_isoch_transfer_partial(IRP *irp, URB *urb)
{
	const struct _URB_ISOCH_TRANSFER *r = &urb->UrbIsochronousTransfer;

	ULONG sz = get_payload_size(r);
	void *dst = try_get_irp_buffer(irp, sz);

	return dst ? copy_payload(dst, irp, r, sz) : STATUS_BUFFER_TOO_SMALL;
}

/*
 * See: <linux>/drivers/usb/usbip/stub_rx.c, is_reset_device_cmd.
 */
static PAGEABLE NTSTATUS usb_reset_port(IRP *irp, struct urb_req *urbr)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_OUT;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, 0);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_RT_PORT; // USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER
	pkt->bRequest = USB_REQUEST_SET_FEATURE;
	pkt->wValue.W = USB_PORT_FEAT_RESET;

	TRANSFERRED(irp) = sizeof(*hdr);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS get_descriptor_from_node_connection(IRP *irp, struct urb_req *urbr)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	const USB_DESCRIPTOR_REQUEST *r = urbr->irp->AssociatedIrp.SystemBuffer;

	IO_STACK_LOCATION *irpstack = IoGetCurrentIrpStackLocation(urbr->irp);
	ULONG data_len = irpstack->Parameters.DeviceIoControl.OutputBufferLength - sizeof(*r); // length of r->Data[]

	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_SHORT_TRANSFER_OK | USBD_TRANSFER_DIRECTION_IN;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, data_len);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
	pkt->bRequest = USB_REQUEST_GET_DESCRIPTOR;
	pkt->wValue.W = r->SetupPacket.wValue;
	pkt->wIndex.W = r->SetupPacket.wIndex;
	pkt->wLength = r->SetupPacket.wLength;

	TRANSFERRED(irp) = sizeof(*hdr);
	return STATUS_SUCCESS;
}

/* 
 * Any URBs queued for such an endpoint should normally be unlinked by the driver before clearing the halt condition, 
 * as described in sections 5.7.5 and 5.8.5 of the USB 2.0 spec.
 * 
 * Thus, a driver must call URB_FUNCTION_ABORT_PIPE before URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL.
 * For that reason vhci_ioctl_abort_pipe(urbr->vpdo, r->PipeHandle) is not called here.
 * 
 * Linux server catches control transfer USB_REQ_CLEAR_FEATURE/USB_ENDPOINT_HALT and calls usb_clear_halt which 
 * a) Issues USB_REQ_CLEAR_FEATURE/USB_ENDPOINT_HALT # URB_FUNCTION_SYNC_CLEAR_STALL
 * b) Calls usb_reset_endpoint # URB_FUNCTION_SYNC_RESET_PIPE
 * 
 * See: <linux>/drivers/usb/usbip/stub_rx.c, is_clear_halt_cmd
        <linux>/drivers/usb/core/message.c, usb_clear_halt
 */
static PAGEABLE NTSTATUS sync_reset_pipe_and_clear_stall(IRP *irp, URB *urb, struct urb_req *urbr)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	struct _URB_PIPE_REQUEST *r = &urb->UrbPipeRequest;
	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_OUT;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, 0);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT;
	pkt->bRequest = USB_REQUEST_CLEAR_FEATURE;
	pkt->wValue.W = USB_FEATURE_ENDPOINT_STALL; // USB_ENDPOINT_HALT
	pkt->wIndex.W = get_endpoint_address(r->PipeHandle);

	TRANSFERRED(irp) = sizeof(*hdr);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS urb_control_descriptor_request(IRP *irp, URB *urb, struct urb_req *urbr, bool dir_in, UCHAR recipient)
{
	ULONG buf_sz = get_irp_buffer_size(irp);

	struct usbip_header *hdr = get_irp_buffer_hdr(irp, buf_sz);
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	struct _URB_CONTROL_DESCRIPTOR_REQUEST *r = &urb->UrbControlDescriptorRequest;

	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_SHORT_TRANSFER_OK | 
					(dir_in ? USBD_TRANSFER_DIRECTION_IN : USBD_TRANSFER_DIRECTION_OUT);

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, r->TransferBufferLength);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = (dir_in ? USB_DIR_IN : USB_DIR_OUT) | USB_TYPE_STANDARD | recipient;
	pkt->bRequest = dir_in ? USB_REQUEST_GET_DESCRIPTOR : USB_REQUEST_SET_DESCRIPTOR;
	pkt->wValue.W = USB_DESCRIPTOR_MAKE_TYPE_AND_INDEX(r->DescriptorType, r->Index); 
	pkt->wIndex.W = r->LanguageId; // relevant for USB_STRING_DESCRIPTOR_TYPE only
	pkt->wLength = (USHORT)r->TransferBufferLength;

	TRANSFERRED(irp) = sizeof(*hdr);

	if (dir_in || !r->TransferBufferLength) {
		return STATUS_SUCCESS;
	}
	
	if (buf_sz - TRANSFERRED(irp) >= r->TransferBufferLength) {
		return copy_transfer_buffer(hdr + 1, urb, irp);
	}

	urbr->vpdo->len_sent_partial = (ULONG)TRANSFERRED(irp);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS urb_control_get_status_request(IRP *irp, URB *urb, struct urb_req *urbr, UCHAR recipient)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	{
		char buf[URB_REQ_STR_BUFSZ];
		TraceInfo(TRACE_READ, "%s: %s", urb_function_str(urb->UrbHeader.Function), urb_req_str(buf, sizeof(buf), urbr));
	}

	struct _URB_CONTROL_GET_STATUS_REQUEST *r = &urb->UrbControlGetStatusRequest;
	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_IN;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, r->TransferBufferLength);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_DIR_IN | USB_TYPE_STANDARD | recipient;
	pkt->bRequest = USB_REQUEST_GET_STATUS;
	pkt->wIndex.W = r->Index;
	pkt->wLength = (USHORT)r->TransferBufferLength; // must be 2
	
	TRANSFERRED(irp) = sizeof(*hdr);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS urb_control_vendor_class_request(IRP *irp, URB *urb, struct urb_req *urbr, UCHAR type, UCHAR recipient)
{
	ULONG buf_sz = get_irp_buffer_size(irp);

	struct usbip_header *hdr = get_irp_buffer_hdr(irp, buf_sz);
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *r = &urb->UrbControlVendorClassRequest;
	bool dir_in = IsTransferDirectionIn(r->TransferFlags);

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 
						EP0, r->TransferFlags | USBD_DEFAULT_PIPE_TRANSFER, r->TransferBufferLength);

	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = (dir_in ? USB_DIR_IN : USB_DIR_OUT) | type | recipient;
	pkt->bRequest = r->Request;
	pkt->wValue.W = r->Value;
	pkt->wIndex.W = r->Index;
	pkt->wLength = (USHORT)r->TransferBufferLength;

	TRANSFERRED(irp) = sizeof(*hdr);

	if (dir_in || !r->TransferBufferLength) {
		return STATUS_SUCCESS;
	}

	if (buf_sz - TRANSFERRED(irp) >= r->TransferBufferLength) {
		return copy_transfer_buffer(hdr + 1, urb, irp);
	}

	urbr->vpdo->len_sent_partial = (ULONG)TRANSFERRED(irp);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS vendor_device(IRP *irp, URB *urb, struct urb_req *urbr)
{
	return urb_control_vendor_class_request(irp, urb, urbr, USB_TYPE_VENDOR, USB_RECIP_DEVICE);
}

static PAGEABLE NTSTATUS vendor_interface(IRP *irp, URB *urb, struct urb_req *urbr)
{
	return urb_control_vendor_class_request(irp, urb, urbr, USB_TYPE_VENDOR, USB_RECIP_INTERFACE);
}

static PAGEABLE NTSTATUS vendor_endpoint(IRP *irp, URB *urb, struct urb_req *urbr)
{
	return urb_control_vendor_class_request(irp, urb, urbr, USB_TYPE_VENDOR, USB_RECIP_ENDPOINT);
}

static PAGEABLE NTSTATUS vendor_other(IRP *irp, URB *urb, struct urb_req *urbr)
{
	return urb_control_vendor_class_request(irp, urb, urbr, USB_TYPE_VENDOR, USB_RECIP_OTHER);
}

static PAGEABLE NTSTATUS class_device(IRP *irp, URB *urb, struct urb_req *urbr)
{
	return urb_control_vendor_class_request(irp, urb, urbr, USB_TYPE_CLASS, USB_RECIP_DEVICE);
}

static PAGEABLE NTSTATUS class_interface(IRP *irp, URB *urb, struct urb_req *urbr)
{
	return urb_control_vendor_class_request(irp, urb, urbr, USB_TYPE_CLASS, USB_RECIP_INTERFACE);
}

static PAGEABLE NTSTATUS class_endpoint(IRP *irp, URB *urb, struct urb_req *urbr)
{
	return urb_control_vendor_class_request(irp, urb, urbr, USB_TYPE_CLASS, USB_RECIP_ENDPOINT);
}

static PAGEABLE NTSTATUS class_other(IRP *irp, URB *urb, struct urb_req *urbr)
{
	return urb_control_vendor_class_request(irp, urb, urbr, USB_TYPE_CLASS, USB_RECIP_OTHER);
}

static PAGEABLE NTSTATUS urb_select_configuration(IRP *irp, URB *urb, struct urb_req *urbr)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	struct _URB_SELECT_CONFIGURATION *r = &urb->UrbSelectConfiguration;
	USB_CONFIGURATION_DESCRIPTOR *cd = r->ConfigurationDescriptor; // NULL if unconfigured

	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_OUT;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, 0);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
	pkt->bRequest = USB_REQUEST_SET_CONFIGURATION;
	pkt->wValue.W = cd ? cd->bConfigurationValue : 0;

	TRANSFERRED(irp) = sizeof(*hdr);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS urb_select_interface(IRP *irp, URB *urb, struct urb_req *urbr)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	struct _URB_SELECT_INTERFACE *r = &urb->UrbSelectInterface;
	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_OUT;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, 0);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
	pkt->bRequest = USB_REQUEST_SET_INTERFACE;
	pkt->wValue.W = r->Interface.AlternateSetting;
	pkt->wIndex.W = r->Interface.InterfaceNumber;

	TRANSFERRED(irp) = sizeof(*hdr);
	return  STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS urb_bulk_or_interrupt_transfer(IRP *irp, URB *urb, struct urb_req *urbr)
{
	struct _URB_BULK_OR_INTERRUPT_TRANSFER *r = &urb->UrbBulkOrInterruptTransfer;
	USBD_PIPE_TYPE type = get_endpoint_type(r->PipeHandle);

	if (!(type == UsbdPipeTypeBulk || type == UsbdPipeTypeInterrupt)) {
		TraceError(TRACE_READ, "%!USBD_PIPE_TYPE!", type);
		return STATUS_INVALID_PARAMETER;
	}

	ULONG buf_sz = get_irp_buffer_size(irp);

	struct usbip_header *hdr = get_irp_buffer_hdr(irp, buf_sz);
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 
							r->PipeHandle, r->TransferFlags, r->TransferBufferLength);

	if (err) {
		return err;
	}

	TRANSFERRED(irp) = sizeof(*hdr);

	if (!r->TransferBufferLength || IsTransferDirectionIn(r->TransferFlags)) {
		return STATUS_SUCCESS;
	}

	if (buf_sz - TRANSFERRED(irp) >= r->TransferBufferLength) {
		return copy_transfer_buffer(hdr + 1, urb, irp);
	}

	urbr->vpdo->len_sent_partial = (ULONG)TRANSFERRED(irp);
	return STATUS_SUCCESS;
}

/*
 * USBD_START_ISO_TRANSFER_ASAP is appended because _URB_GET_CURRENT_FRAME_NUMBER is not implemented.
 */
static PAGEABLE NTSTATUS urb_isoch_transfer(IRP *irp, URB *urb, struct urb_req *urbr)
{
	struct _URB_ISOCH_TRANSFER *r = &urb->UrbIsochronousTransfer;
	USBD_PIPE_TYPE type = get_endpoint_type(r->PipeHandle);

	if (type != UsbdPipeTypeIsochronous) {
		TraceError(TRACE_READ, "%!USBD_PIPE_TYPE!", type);
		return STATUS_INVALID_PARAMETER;
	}

	ULONG buf_sz = get_irp_buffer_size(irp);

	struct usbip_header *hdr = get_irp_buffer_hdr(irp, buf_sz);
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 
					r->PipeHandle, r->TransferFlags | USBD_START_ISO_TRANSFER_ASAP, r->TransferBufferLength);

	if (err) {
		return err;
	}

	hdr->u.cmd_submit.start_frame = r->StartFrame;
	hdr->u.cmd_submit.number_of_packets = r->NumberOfPackets;

	TRANSFERRED(irp) = sizeof(*hdr);
	ULONG sz = get_payload_size(r);

	if (buf_sz - TRANSFERRED(irp) >= sz) {
		return copy_payload(hdr + 1, irp, r, sz);
	}

	urbr->vpdo->len_sent_partial = (ULONG)TRANSFERRED(irp);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS urb_control_transfer_any(IRP *irp, URB *urb, struct urb_req* urbr)
{
	struct _URB_CONTROL_TRANSFER *r = &urb->UrbControlTransfer;
	static_assert(offsetof(struct _URB_CONTROL_TRANSFER, SetupPacket) == offsetof(struct _URB_CONTROL_TRANSFER_EX, SetupPacket), "assert");

	bool dir_out = is_transfer_dir_out(r);
	ULONG buf_sz = get_irp_buffer_size(irp);

	struct usbip_header *hdr = get_irp_buffer_hdr(irp, buf_sz);
	if (!hdr) {
		TraceError(TRACE_READ, "Cannot get usbip header");
		return STATUS_BUFFER_TOO_SMALL;
	}

	if (dir_out != IsTransferDirectionOut(r->TransferFlags)) {
		TraceError(TRACE_READ, "Transfer direction differs in TransferFlags(%#lx) and SetupPacket", r->TransferFlags);
		return STATUS_INVALID_PARAMETER;
	}

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, 
							r->PipeHandle, r->TransferFlags, r->TransferBufferLength);

	if (err) {
		return err;
	}

	static_assert(sizeof(hdr->u.cmd_submit.setup) == sizeof(r->SetupPacket), "assert");
	RtlCopyMemory(hdr->u.cmd_submit.setup, r->SetupPacket, sizeof(hdr->u.cmd_submit.setup));

	TRANSFERRED(irp) = sizeof(*hdr);

	if (!(dir_out && r->TransferBufferLength)) {
		return STATUS_SUCCESS;
	}

	if (buf_sz - TRANSFERRED(irp) >= r->TransferBufferLength) {
		return copy_transfer_buffer(hdr + 1, urb, irp);
	}

	urbr->vpdo->len_sent_partial = (ULONG)TRANSFERRED(irp);
	return STATUS_SUCCESS;
}

/*
 * vhci_internal_ioctl.c handles such functions itself.
 */
static PAGEABLE NTSTATUS urb_function_unexpected(IRP *irp, URB *urb, struct urb_req* urbr)
{
	UNREFERENCED_PARAMETER(urbr);

	USHORT func = urb->UrbHeader.Function;
	TraceError(TRACE_READ, "%s(%#04x) must never be called, internal logic error", urb_function_str(func), func);

	NT_ASSERT(!TRANSFERRED(irp));
	return STATUS_INTERNAL_ERROR;
}	

static PAGEABLE NTSTATUS get_descriptor_from_device(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_descriptor_request(irp, urb, urbr, USB_DIR_IN, USB_RECIP_DEVICE);
}

static PAGEABLE NTSTATUS set_descriptor_to_device(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_descriptor_request(irp, urb, urbr, USB_DIR_OUT, USB_RECIP_DEVICE);
}

static PAGEABLE NTSTATUS get_descriptor_from_interface(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_descriptor_request(irp, urb, urbr, USB_DIR_IN, USB_RECIP_INTERFACE);
}

static PAGEABLE NTSTATUS set_descriptor_to_interface(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_descriptor_request(irp, urb, urbr, USB_DIR_OUT, USB_RECIP_INTERFACE);
}

static PAGEABLE NTSTATUS get_descriptor_from_endpoint(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_descriptor_request(irp, urb, urbr, USB_DIR_IN, USB_RECIP_ENDPOINT);
}

static PAGEABLE NTSTATUS set_descriptor_to_endpoint(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_descriptor_request(irp, urb, urbr, USB_DIR_OUT, USB_RECIP_ENDPOINT);
}

static PAGEABLE NTSTATUS urb_control_feature_request(IRP *irp, URB *urb, struct urb_req* urbr, UCHAR bRequest, UCHAR recipient)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	struct _URB_CONTROL_FEATURE_REQUEST *r = &urb->UrbControlFeatureRequest;
	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_OUT;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, 0);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_DIR_OUT | USB_TYPE_STANDARD | recipient;
	pkt->bRequest = bRequest;
	pkt->wValue.W = r->FeatureSelector; 
	pkt->wIndex.W = r->Index;

	TRANSFERRED(irp) = sizeof(*hdr);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS set_feature_to_device(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_feature_request(irp, urb, urbr, USB_REQUEST_SET_FEATURE, USB_RECIP_DEVICE);
}

static PAGEABLE NTSTATUS set_feature_to_interface(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_feature_request(irp, urb, urbr, USB_REQUEST_SET_FEATURE, USB_RECIP_INTERFACE);
}

static PAGEABLE NTSTATUS set_feature_to_endpoint(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_feature_request(irp, urb, urbr, USB_REQUEST_SET_FEATURE, USB_RECIP_ENDPOINT);
}

static PAGEABLE NTSTATUS set_feature_to_other(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_feature_request(irp, urb, urbr, USB_REQUEST_SET_FEATURE, USB_RECIP_OTHER);
}

static PAGEABLE NTSTATUS clear_feature_to_device(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_feature_request(irp, urb, urbr, USB_REQUEST_CLEAR_FEATURE, USB_RECIP_DEVICE);
}

static PAGEABLE NTSTATUS clear_feature_to_interface(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_feature_request(irp, urb, urbr, USB_REQUEST_CLEAR_FEATURE, USB_RECIP_INTERFACE);
}

static PAGEABLE NTSTATUS clear_feature_to_endpoint(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_feature_request(irp, urb, urbr, USB_REQUEST_CLEAR_FEATURE, USB_RECIP_ENDPOINT);
}

static PAGEABLE NTSTATUS clear_feature_to_other(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_feature_request(irp, urb, urbr, USB_REQUEST_CLEAR_FEATURE, USB_RECIP_OTHER);
}

static PAGEABLE NTSTATUS get_configuration(IRP *irp, URB *urb, struct urb_req* urbr)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	struct _URB_CONTROL_GET_CONFIGURATION_REQUEST *r = &urb->UrbControlGetConfigurationRequest;
	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_IN;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, r->TransferBufferLength);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE;
	pkt->bRequest = USB_REQUEST_GET_CONFIGURATION;
	pkt->wLength = (USHORT)r->TransferBufferLength; // must be 1

	TRANSFERRED(irp) = sizeof(*hdr);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS get_interface(IRP *irp, URB *urb, struct urb_req* urbr)
{
	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_BUFFER_TOO_SMALL;
	}

	struct _URB_CONTROL_GET_INTERFACE_REQUEST *r = &urb->UrbControlGetInterfaceRequest;
	const ULONG TransferFlags = USBD_DEFAULT_PIPE_TRANSFER | USBD_TRANSFER_DIRECTION_IN;

	NTSTATUS err = set_cmd_submit_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, EP0, TransferFlags, r->TransferBufferLength);
	if (err) {
		return err;
	}

	USB_DEFAULT_PIPE_SETUP_PACKET *pkt = get_submit_setup(hdr);
	pkt->bmRequestType.B = USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE;
	pkt->bRequest = USB_REQUEST_GET_INTERFACE;
	pkt->wIndex.W = r->Interface;
	pkt->wLength = (USHORT)r->TransferBufferLength; // must be 1

	TRANSFERRED(irp) = sizeof(*hdr);
	return STATUS_SUCCESS;
}

static PAGEABLE NTSTATUS get_status_from_device(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_get_status_request(irp, urb, urbr, USB_RECIP_DEVICE);
}

static PAGEABLE NTSTATUS get_status_from_interface(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_get_status_request(irp, urb, urbr, USB_RECIP_INTERFACE);
}

static PAGEABLE NTSTATUS get_status_from_endpoint(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_get_status_request(irp, urb, urbr, USB_RECIP_ENDPOINT);
}

static PAGEABLE NTSTATUS get_status_from_other(IRP *irp, URB *urb, struct urb_req* urbr)
{
	return urb_control_get_status_request(irp, urb, urbr, USB_RECIP_OTHER);
}

typedef NTSTATUS (*urb_function_t)(IRP *irp, URB *urb, struct urb_req*);

static const urb_function_t urb_functions[] =
{
	urb_select_configuration,
	urb_select_interface,
	urb_function_unexpected, // URB_FUNCTION_ABORT_PIPE, urb_pipe_request

	urb_function_unexpected, // URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL
	urb_function_unexpected, // URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL

	urb_function_unexpected, // URB_FUNCTION_GET_FRAME_LENGTH
	urb_function_unexpected, // URB_FUNCTION_SET_FRAME_LENGTH
	urb_function_unexpected, // URB_FUNCTION_GET_CURRENT_FRAME_NUMBER

	urb_control_transfer_any,
	urb_bulk_or_interrupt_transfer,
	urb_isoch_transfer,

	get_descriptor_from_device,
	set_descriptor_to_device,

	set_feature_to_device,
	set_feature_to_interface, 
	set_feature_to_endpoint,

	clear_feature_to_device,
	clear_feature_to_interface,
	clear_feature_to_endpoint,

	get_status_from_device,
	get_status_from_interface,
	get_status_from_endpoint,

	NULL, // URB_FUNCTION_RESERVED_0X0016          

	vendor_device,
	vendor_interface,
	vendor_endpoint,

	class_device,
	class_interface,
	class_endpoint,

	NULL, // URB_FUNCTION_RESERVE_0X001D

	sync_reset_pipe_and_clear_stall, // urb_pipe_request

	class_other,
	vendor_other,

	get_status_from_other,

	set_feature_to_other, 
	clear_feature_to_other,

	get_descriptor_from_endpoint,
	set_descriptor_to_endpoint,

	get_configuration, // URB_FUNCTION_GET_CONFIGURATION
	get_interface, // URB_FUNCTION_GET_INTERFACE

	get_descriptor_from_interface,
	set_descriptor_to_interface,

	urb_function_unexpected, // URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR

	NULL, // URB_FUNCTION_RESERVE_0X002B
	NULL, // URB_FUNCTION_RESERVE_0X002C
	NULL, // URB_FUNCTION_RESERVE_0X002D
	NULL, // URB_FUNCTION_RESERVE_0X002E
	NULL, // URB_FUNCTION_RESERVE_0X002F

	urb_function_unexpected, // URB_FUNCTION_SYNC_RESET_PIPE, urb_pipe_request
	urb_function_unexpected, // URB_FUNCTION_SYNC_CLEAR_STALL, urb_pipe_request
	urb_control_transfer_any, // URB_FUNCTION_CONTROL_TRANSFER_EX

	NULL, // URB_FUNCTION_RESERVE_0X0033
	NULL, // URB_FUNCTION_RESERVE_0X0034                  

	urb_function_unexpected, // URB_FUNCTION_OPEN_STATIC_STREAMS
	urb_function_unexpected, // URB_FUNCTION_CLOSE_STATIC_STREAMS, urb_pipe_request
	urb_bulk_or_interrupt_transfer, // URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER_USING_CHAINED_MDL
	urb_isoch_transfer, // URB_FUNCTION_ISOCH_TRANSFER_USING_CHAINED_MDL

	NULL, // 0x0039
	NULL, // 0x003A        
	NULL, // 0x003B        
	NULL, // 0x003C        

	urb_function_unexpected // URB_FUNCTION_GET_ISOCH_PIPE_TRANSFER_PATH_DELAYS
};

static PAGEABLE NTSTATUS usb_submit_urb(IRP *irp, struct urb_req *urbr)
{
	URB *urb = URB_FROM_IRP(urbr->irp);
	if (!urb) {
		TraceError(TRACE_READ, "null urb");
		return STATUS_INVALID_DEVICE_REQUEST;
	}

	USHORT func = urb->UrbHeader.Function;

	urb_function_t pfunc = func < ARRAYSIZE(urb_functions) ? urb_functions[func] : NULL;
	if (pfunc) {
		return pfunc(irp, urb, urbr);
	}

	TraceError(TRACE_READ, "%s(%#04x) has no handler (reserved?)", urb_function_str(func), func);
	return STATUS_INVALID_PARAMETER;
}

static PAGEABLE NTSTATUS store_urbr_partial(IRP *read_irp, struct urb_req *urbr)
{
	{
		char buf[URB_REQ_STR_BUFSZ];
		TraceVerbose(TRACE_READ, "Enter %s", urb_req_str(buf, sizeof(buf), urbr));
	}

	URB *urb = URB_FROM_IRP(urbr->irp);
	NTSTATUS status = STATUS_INVALID_PARAMETER;

	switch (urb->UrbHeader.Function) {
	case URB_FUNCTION_ISOCH_TRANSFER:
		status = urb_isoch_transfer_partial(read_irp, urb);
		break;
	case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
	case URB_FUNCTION_CONTROL_TRANSFER:
	case URB_FUNCTION_CONTROL_TRANSFER_EX:
	case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
	case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
	case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT:
	case URB_FUNCTION_CLASS_DEVICE:
	case URB_FUNCTION_CLASS_INTERFACE:
	case URB_FUNCTION_CLASS_ENDPOINT:
	case URB_FUNCTION_CLASS_OTHER:
	case URB_FUNCTION_VENDOR_DEVICE:
	case URB_FUNCTION_VENDOR_INTERFACE:
	case URB_FUNCTION_VENDOR_ENDPOINT:
	case URB_FUNCTION_VENDOR_OTHER:
		status = transfer_partial(read_irp, urb);
		break;
	default:
		TraceError(TRACE_READ, "%s: unexpected partial urbr", urb_function_str(urb->UrbHeader.Function));
	}

	if (!status) {
		struct usbip_header *hdr = try_get_irp_buffer(read_irp, sizeof(*hdr));
		size_t sz = get_pdu_payload_size(hdr);
		size_t transferred = TRANSFERRED(read_irp);
		if (sz != transferred) {
			TraceError(TRACE_READ, "pdu payload size %Iu != transferred %Iu", sz, transferred);
		}
	}

	TraceVerbose(TRACE_READ, "Leave %!STATUS!", status);
	return status;
}

static PAGEABLE NTSTATUS store_cancelled_urbr(PIRP irp, struct urb_req *urbr)
{
	TraceInfo(TRACE_READ, "Enter");

	struct usbip_header *hdr = try_get_irp_buffer(irp, sizeof(*hdr));
	if (!hdr) {
		return STATUS_INVALID_PARAMETER;
	}

	set_cmd_unlink_usbip_header(hdr, urbr->seq_num, urbr->vpdo->devid, urbr->seq_num_unlink);

	TRANSFERRED(irp) += sizeof(*hdr);
	return STATUS_SUCCESS;
}

NTSTATUS store_urbr(IRP *read_irp, struct urb_req *urbr)
{
	if (!urbr->irp) {
		return store_cancelled_urbr(read_irp, urbr);
	}

	NTSTATUS err = STATUS_INVALID_PARAMETER;

	IO_STACK_LOCATION *irpstack = IoGetCurrentIrpStackLocation(urbr->irp);
	ULONG ioctl_code = irpstack->Parameters.DeviceIoControl.IoControlCode;

	switch (ioctl_code) {
	case IOCTL_INTERNAL_USB_SUBMIT_URB:
		err = usb_submit_urb(read_irp, urbr);
		break;
	case IOCTL_INTERNAL_USB_RESET_PORT:
		err = usb_reset_port(read_irp, urbr);
		break;
	case IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION:
		err = get_descriptor_from_node_connection(read_irp, urbr);
		break;
	default:
		TraceWarning(TRACE_READ, "unhandled %s(%#08lX)", dbg_ioctl_code(ioctl_code), ioctl_code);
	}

	if (!err) {
		const struct usbip_header *hdr = get_irp_buffer(read_irp);
		char buf[DBG_USBIP_HDR_BUFSZ];
		TraceInfo(TRACE_READ, "%s", dbg_usbip_hdr(buf, sizeof(buf), hdr));	
	}

	return err;
}

static PAGEABLE void on_pending_irp_read_cancelled(DEVICE_OBJECT *devobj, IRP *irp_read)
{
	UNREFERENCED_PARAMETER(devobj);
	PAGED_CODE();

	TraceInfo(TRACE_READ, "pending irp read cancelled %p", irp_read);

	IoReleaseCancelSpinLock(irp_read->CancelIrql);

	IO_STACK_LOCATION *irpstack = IoGetCurrentIrpStackLocation(irp_read);
	vpdo_dev_t *vpdo = irpstack->FileObject->FsContext;

	KIRQL irql;
	KeAcquireSpinLock(&vpdo->lock_urbr, &irql);
	if (vpdo->pending_read_irp == irp_read) {
		vpdo->pending_read_irp = NULL;
	}
	KeReleaseSpinLock(&vpdo->lock_urbr, irql);

	irp_done(irp_read, STATUS_CANCELLED);
}

static PAGEABLE NTSTATUS process_read_irp(vpdo_dev_t *vpdo, IRP *read_irp)
{
	NTSTATUS status = STATUS_SUCCESS;
	struct urb_req *urbr = NULL;
	KIRQL oldirql;

	KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);

	if (vpdo->pending_read_irp) {
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
		return STATUS_INVALID_DEVICE_REQUEST;
	}

	if (vpdo->urbr_sent_partial) {
		NT_ASSERT(vpdo->len_sent_partial);
		urbr = vpdo->urbr_sent_partial;

		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
		status = store_urbr_partial(read_irp, urbr);
		KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);

		vpdo->len_sent_partial = 0;
	} else {
		NT_ASSERT(!vpdo->len_sent_partial);

		urbr = find_pending_urbr(vpdo);
		if (!urbr) {
			vpdo->pending_read_irp = read_irp;

			KIRQL oldirql_cancel;
			IoAcquireCancelSpinLock(&oldirql_cancel);
			IoSetCancelRoutine(read_irp, on_pending_irp_read_cancelled);
			IoReleaseCancelSpinLock(oldirql_cancel);
			KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
			IoMarkIrpPending(read_irp);

			return STATUS_PENDING;
		}

		vpdo->urbr_sent_partial = urbr;
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

		status = store_urbr(read_irp, urbr);

		KeAcquireSpinLock(&vpdo->lock_urbr, &oldirql);
	}

	if (status != STATUS_SUCCESS) {
		RemoveEntryListInit(&urbr->list_all);
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);

		PIRP irp = urbr->irp;
		free_urbr(urbr);

		if (irp) {
			// urbr irp has cancel routine, if the IoSetCancelRoutine returns NULL that means IRP was cancelled
			IoAcquireCancelSpinLock(&oldirql);
			bool valid = IoSetCancelRoutine(irp, NULL);
			IoReleaseCancelSpinLock(oldirql);
			if (valid) {
				TRANSFERRED(irp) = 0;
				irp_done(irp, STATUS_INVALID_PARAMETER);
			}
		}
	} else {
		if (!vpdo->len_sent_partial) {
			InsertTailList(&vpdo->head_urbr_sent, &urbr->list_state);
			vpdo->urbr_sent_partial = NULL;
		}
		KeReleaseSpinLock(&vpdo->lock_urbr, oldirql);
	}

	return status;
}

/*
* ReadFile -> IRP_MJ_READ -> vhci_read
*/
PAGEABLE NTSTATUS vhci_read(__in DEVICE_OBJECT *devobj, __in IRP *irp)
{
	PAGED_CODE();
	NT_ASSERT(!TRANSFERRED(irp));

	TraceVerbose(TRACE_READ, "Enter irql %!irql!, read buffer %lu", KeGetCurrentIrql(), get_irp_buffer_size(irp));

	vhci_dev_t *vhci = devobj_to_vhci_or_null(devobj);
	if (!vhci) {
		TraceError(TRACE_READ, "read for non-vhci is not allowed");
		return  irp_done(irp, STATUS_INVALID_DEVICE_REQUEST);
	}

	NTSTATUS status = STATUS_NO_SUCH_DEVICE;

	if (vhci->common.DevicePnPState != Deleted) {
		IO_STACK_LOCATION *irpstack = IoGetCurrentIrpStackLocation(irp);
		vpdo_dev_t *vpdo = irpstack->FileObject->FsContext;
		status = vpdo && vpdo->plugged ? process_read_irp(vpdo, irp) : STATUS_INVALID_DEVICE_REQUEST;
	}

	if (status != STATUS_PENDING) {
		irp_done(irp, status);
	}

	TraceVerbose(TRACE_READ, "Leave %!STATUS!, transferred %Iu", status, TRANSFERRED(irp));
	return status;
}
