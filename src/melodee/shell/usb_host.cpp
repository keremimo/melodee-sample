// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Markus Sunela
#include "melodee/shell/usb_host.h"
#include "drivers/kernel_module.h"
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace melodee {

// Anonymous namespace for the file-private functions
namespace {
namespace fs = std::filesystem;

// Find all available file interfaces for controlling the USB mode for MPC Sample.
// (/sys/kernel/debug/usb/<controller>.usb/mode, "device"/"host"/"otg" on the
// RK3566's fcc00000.usb).
std::vector<fs::path> role_files() {
    std::vector<fs::path> out;
    std::error_code ec;

    // Usually the USB role should be available via /sys/class/usb_role/role
    for (const auto& entry : fs::directory_iterator("/sys/class/usb_role", ec))
        out.push_back(entry.path() / "role");
    // ...but on MPC Sample it is not available. Use debugfs /sys/kernel/debug/usb/*.usb/mode
    // as it works just fine for the purpose.
    for (const auto& entry : fs::directory_iterator("/sys/kernel/debug/usb", ec))
        if (entry.path().extension() == ".usb") out.push_back(entry.path() / "mode");

    return out;
}

// Get the gadget's UDC binding, so a switch to host can unbind it first, as the controller, dwc3,
// refuses the switch while gadget is bound. Turning host mode off again can then bind it back.
std::pair<fs::path, std::string> gadget_udc() {
    std::error_code ec;
    for (const auto& g : fs::directory_iterator("/sys/kernel/config/usb_gadget", ec))
        if (fs::is_directory(g.path() / "UDC")) {
            std::ifstream in(g.path() / "UDC");
            std::string udc;
            if (in >> udc && !udc.empty()) return {g.path() / "UDC", udc};
        }
    return {};
}

} // namespace

// Returns active USB role: 'on' (USB host), 'otg' or 'off' (gadget/peripheral).
std::string usb_host_status() {
    // Read the current role from the first role file that completes ok.
    std::string role_read = "";
    for (const auto& file : role_files()) {
        std::ifstream role_file(file);
        if (role_file >> role_read)
            break;
    }
    if (role_read.empty())
        return "ERR no role file could be read";

    std::string role = role_read == "host" ? "on" : (role_read == "otg" ? "otg" : "peripheral");
    return "OK " + role;
}

// Change USB role to 'on' (USB host), 'otg' or 'off' (gadget/peripheral).
std::string usb_host_set(const std::string& mode) {
    // Sanitize the incoming mode to a role to use
    if (mode != "on" && mode != "off" && mode != "otg")
        return "ERR usb-host mode on|off|otg";
    const char* role = mode == "on" ? "host" : (mode == "otg" ? "otg" : "peripheral");

    // Probe the device-class modules first. The host controller itself is built into MPC Sample kernel
    // (config: XHCI/DWC3 dual-role/role-switch/OTG/PHYs all are 'y'); the device classes are the modular part.
    std::string error;
    kernel_module::probe({"snd-usb-audio", "usb-storage", "uas", "usbnet", "cdc_ether", "cdc_ncm"}, &error);

    // Unbind the gadget when enabling host/otg (the dwc3 refuses a role
    // change while bound). Bind it back when going peripheral.
    auto gadget = gadget_udc();
    bool was_bound = !gadget.second.empty();
    if (was_bound && mode != "off") {
        std::ofstream unbind(gadget.first);
        unbind << "\n";
    }

    // Write the new role to all the role files and keep track on what
    // writes succeeded and what not.
    std::string roles_written = "", roles_failed = "";
    for (const auto& file : role_files()) {
        std::string filename = file.parent_path().filename().string();
        std::ofstream out(file);
        if (out << role)
            roles_written += (roles_written.empty() ? "" : ",") + filename;
        else
            roles_failed += (roles_failed.empty() ? "" : ",") + filename;
    }
    if (roles_written.empty()) {
        if (was_bound && mode != "off") {
            std::ofstream rebind(gadget.first);
            rebind << gadget.second << "\n";
        }
        return "ERR no role file could be written to" + roles_failed;
    }

    // Keep the bus and the connected devices awake: no USB autosuspend in host mode,
    // otherwise the connected devices will periodically lose power.
    std::string awake;
    if (mode != "off") {
        // Disable autosuspend on the bus & devices connected later
        std::ofstream core("/sys/module/usbcore/parameters/autosuspend");
        if (core << "-1\n")
            awake = "host";

        // Disable autosuspend for already connected devices
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator("/sys/bus/usb/devices", ec)) {
            std::ofstream ctl(entry.path() / "power" / "control");
            if (ctl << "on\n")
                awake += (awake.empty() ? "" : ",") + entry.path().filename().string();
        }
    }

    // Rebind the gadget again, when host mode is turned off
    if (mode == "off" && was_bound) {
        std::ofstream rebind(gadget.first);
        rebind << gadget.second << "\n";
    }

    // Return the current state
    return "OK" + std::string(role);
}

} // namespace melodee
