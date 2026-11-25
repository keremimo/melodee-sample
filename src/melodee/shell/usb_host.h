// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Markus Sunela
#pragma once
#include <filesystem>
#include <string>

namespace melodee {

// Class for getting and setting MPC Sample's USB-C port role. Settings'
// SYSTEM view uses the functions to get and set the USB port mode.
class UsbHost {
public:
    UsbHost();

    // Returns the current USB role: 'on' (USB host), 'otg' or 'off' (gadget/peripheral).
    // Returns string starting with 'OK ' on success and 'ERR ' on error. Rest
    // of the return value is the status or the error message.
    std::string usb_host_status();

    // Changes USB role to 'on' (USB host), 'otg' or 'off' (gadget/peripheral).
    // Returns string starting with 'OK ' on success and 'ERR ' on error. Rest
    // of the return value is the new status or the error message.
    std::string usb_host_set(const std::string& mode);
private:
    // Reads the active USB mode from the role file.
    // Returns empty string, if the role file cannot be read.
    static std::string read_usb_mode();

    // Changes the USB mode (on|off|otg) by writing the matching role to the
    // role file. Returns true, if role change write succeeds and the set
    // role matches the requested one.
    static bool write_usb_mode(const std::string& mode);
};

} // namespace melodee
