#ifndef USB_COMMANDS_H
#define USB_COMMANDS_H

// Handler principal para comandos USB
void cmd_usb(const char* args);

// Subcomandos individuales
void cmd_usb_list(void);
void cmd_usb_storage(void);
void cmd_usb_scan(void);
void cmd_usb_info(const char* device_str);

#endif // USB_COMMANDS_H