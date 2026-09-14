// Speaks grblHAL's line protocol over the USB-host CDC link (usb_host_cdc.*):
// single-line-at-a-time ok/error handshake for queued G-code, out-of-band real-time
// bytes for hold/resume/reset, and periodic '?' status polling.
//
// Runs entirely on core1. Call grbl_link_core1_main() as the multicore_launch_core1
// entry point; it never returns.
#ifndef GRBL_LINK_H
#define GRBL_LINK_H

void grbl_link_core1_main(void);

#endif
