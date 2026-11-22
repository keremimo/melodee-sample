// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Markus Sunela
#pragma once
#include <string>

// Functions for getting and setting MPC Sample's USB-C port role. Settings'
// SYSTEM view uses the functions to get and set the USB port mode and role.
namespace melodee {

    // Returns the current USB role: 'on' (USB host), 'otg' or 'off' (gadget/peripheral).
    // Returns string starting with 'OK ' on success and 'ERR ' on error. Rest
    // of the return value is the status or the error message.
    std::string usb_host_status();

    // Changes USB role to 'on' (USB host), 'otg' or 'off' (gadget/peripheral).
    // Returns string starting with 'OK ' on success and 'ERR ' on error. Rest
    // of the return value is the new status or the error message.
    std::string usb_host_set(const std::string& mode);

} // namespace melodee
