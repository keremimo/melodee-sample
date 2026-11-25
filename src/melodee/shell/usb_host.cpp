// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Markus Sunela
#include "melodee/shell/context.h"
#include "melodee/shell/usb_host.h"
#include "drivers/kernel_module.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace melodee {

namespace fs = std::filesystem;

namespace {
    const std::string usb_gadget = "fcc00000.usb";
    const std::string usb_role_file = "/sys/kernel/debug/usb/" + usb_gadget + "/mode";
    const std::string usb_gadget_file = "/sys/kernel/config/usb_gadget/ac50/UDC";
    const std::string autosuspend_file = "/sys/module/usbcore/parameters/autosuspend";

    bool unbind() {
        std::ofstream unbind_out(usb_gadget_file);
        unbind_out << "\n";
        unbind_out.close();
        return unbind_out.good();
    }

    bool bind() {
        std::ofstream bind_out(usb_gadget_file);
        bind_out << usb_gadget;
        bind_out.close();
        return bind_out.good();
    }

    // Keep the bus and the connected devices awake: no USB autosuspend in host mode,
    // otherwise the connected devices will periodically lose power.
    void disable_autosuspend() {
        // Disable autosuspend on the bus & devices connected later
        std::ofstream core(autosuspend_file);
        core << "-1\n";
        core.close();

        // Disable autosuspend for already connected devices
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/sys/bus/usb/devices", ec)) {
            std::ofstream ctl(entry.path() / "power" / "control");
            ctl << "on\n";
            ctl.close();
        }
    }
}

UsbHost::UsbHost() { }

// Reads the active USB mode from the role file.
// Returns empty string, if the role file cannot be read.
std::string UsbHost::read_usb_mode() {
    std::ifstream role_file(usb_role_file);
    std::string role_read;
    role_file >> role_read;
    role_file.close();
    if (!role_file.good() || role_read.empty())
        return "";
    return role_read == "host" ? "on" : (role_read == "otg" ? "otg" : "off");
}

// Changes the USB mode (on|off|otg) by writing the matching role to the
// role file. Returns true, if role change write succeeds and the set
// role matches the requested one.
bool UsbHost::write_usb_mode(const std::string& mode) {
    // The role to use in Linux API
    const char* role = mode == "on" ? "host" : (mode == "otg" ? "otg" : "device");

    // Try to write the new role to the device
    std::ofstream role_output(usb_role_file);
    role_output << role << "\n";
    role_output.close();
    return role_output.good();
}

// Returns the current USB role: 'on' (USB host), 'otg' or 'off' (gadget/peripheral).
// Returns string starting with 'OK ' on success and 'ERR ' on error. Rest
// of the return value is the status or the error message.
std::string UsbHost::usb_host_status() {
    // Read the current role from the first role file that completes ok.
    std::string mode = read_usb_mode();
    if (mode.empty())
        return "ERR role couldn't be read";
    return "OK " + mode;
}

// Changes USB role to 'on' (USB host), 'otg' or 'off' (gadget/peripheral).
// Returns string starting with 'OK ' on success and 'ERR ' on error. Rest
// of the return value is the new status or the error message.
std::string UsbHost::usb_host_set(const std::string& mode) {
    // Sanitize the incoming mode
    if (mode != "on" && mode != "off" && mode != "otg")
        return "ERR usb-host mode on|off|otg";

    // Probe the device-class modules first. The host controller itself is built into MPC Sample kernel
    // (config: XHCI/DWC3 dual-role/role-switch/OTG/PHYs all are 'y'); the device classes are the modular part.
    if (mode != "off") {
        std::string error;
        kernel_module::probe({"snd-usb-audio", "usb-storage", "uas", "usbnet", "cdc_ether", "cdc_ncm"}, &error);
    }

    // Unbind the gadget when enabling host/otg (the dwc3 refuses a role
    // change while bound). Bind it back when going peripheral again.
    unbind();

    // Allow 200 ms for the unbind to take effect
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Write the new USB mode
    if (!write_usb_mode(mode)) {
        bind();
        return "ERR role couldn't be changed";
    }

    // Allow 500 ms for the mode to take effect
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Disable USB autosuspend
    if (mode != "off")
        disable_autosuspend();

    // Rebind the gadget.
    bind();

    return "OK " + std::string(mode);
}

std::string Context::usb_host_status() {
    return usb_host.usb_host_status();
}

std::string Context::usb_host_set(const std::string& mode) {
    return usb_host.usb_host_set(mode);
}

} // namespace melodee
