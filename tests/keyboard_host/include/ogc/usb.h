/* The parts of libogc's ogc/usb.h that keyboard.c uses. */
#ifndef HOST_USB_H
#define HOST_USB_H
#include <gctypes.h>
#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_ENDPOINT_INTERRUPT 0x03
#define USB_ENDPOINT_IN 0x80
#define USB_REQTYPE_INTERFACE_SET 0x21
#define USB_REQ_SETPROTOCOL 0x0b
typedef struct { u8 bmAttributes, bEndpointAddress; } usb_endpointdesc;
typedef struct {
	u8 bInterfaceNumber, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, bNumEndpoints;
	usb_endpointdesc endpoints[2];
} usb_interfacedesc;
typedef struct { u8 bNumInterfaces; usb_interfacedesc interfaces[2]; } usb_configurationdesc;
typedef struct { u8 bNumConfigurations; usb_configurationdesc configurations[1]; } usb_devdesc;
typedef struct { s32 device_id; u16 vid, pid; u32 token; } usb_device_entry;
typedef s32 (*usbcallback) (s32 result, void *usrdata);
s32 USB_Initialize (void);
s32 USB_OpenDevice (s32 device_id, u16 vid, u16 pid, s32 *fd);
s32 USB_CloseDevice (s32 *fd);
s32 USB_GetDescriptors (s32 fd, usb_devdesc *udd);
void USB_FreeDescriptors (usb_devdesc *udd);
s32 USB_WriteCtrlMsg (s32 fd, u8 bmRequestType, u8 bmRequest, u16 wValue, u16 wIndex, u16 wLength, void *rpData);
s32 USB_ReadIntrMsgAsync (s32 fd, u8 bEndpoint, u16 wLength, void *rpData, usbcallback cb, void *usrdata);
s32 USB_GetDeviceList (usb_device_entry *descr_buffer, u8 num_descr, u8 interface_class, u8 *cnt_descr);
s32 USB_ClearHalt (s32 fd, u8 endpointAddress);
#endif
