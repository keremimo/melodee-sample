// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Updates from the card (docs/MELODEE.md, "Updates from the card"). When a
// newer Melodee release zip is in the card's top folder, Melodee asks
// whether to install it once playback stops. INSTALL saves the project, then
// the base's install/sd-update.sh (drivers/sd_update.h) verifies the zip,
// installs it on the card and restarts Melodee. The service thread finds the
// zip and reads the install status; the MIDI thread shows the question and
// the progress.
namespace melodee {

// This build's version: ./release.sh's VERSION, git describe, or "dev".
const char* melodee_version();

// Public Ed25519 key for verifying release signatures. tools/make_release.py
// also checks each release's signature against it.
constexpr const char* release_key_pem =
    "-----BEGIN PUBLIC KEY-----\n"
    "MCowBQYDK2VwAyEAzMDuCtv5wbUe9SUfL8g5qiu0+/YEYLB/LwjEEqUlX5w=\n"
    "-----END PUBLIC KEY-----\n";

// Install progress (UiState::update.phase).
enum class UpdatePhase : uint8_t { None, Saving, Checking, Writing, Starting, Failed };
// Reason for a failed install, from sd-update.sh's error= value.
enum class UpdateError : uint8_t { None, Signature, Damaged, SelfTest, NoCard, OtherApp, CardFull, CardWrite, NoStart, Busy, Other };
UpdateError update_error(const std::string& word);
const char* update_error_text(UpdateError error); // "SD card is full", for the sheet
inline bool update_running(uint8_t phase) {
    return phase >= uint8_t(UpdatePhase::Saving) && phase <= uint8_t(UpdatePhase::Starting);
}

// Passes update state between the service thread and the MIDI thread
// under a mutex (never used on the audio thread).
class UpdateLink {
public:
    // Service thread: sets the zip to offer; each call is a new offer (empty: no offer).
    void offer(const std::string& zip, const std::string& version);
    // MIDI thread: returns a new offer's version once; offering() is true while an offer is set.
    bool take_offer(std::string& version);
    bool offering();
    // MIDI thread: B1 on the question (LATER), or install after the project is saved.
    void decline();
    void install();
    // Service thread: the request since the last call and its zip (0 none, 1 later, 2 install).
    uint8_t take_request(std::string& zip);
    // Service thread: sets the install progress; the MIDI thread copies it to UiState::update.
    void progress(UpdatePhase phase, UpdateError error);
    UpdatePhase phase() const { return UpdatePhase(phase_now.load(std::memory_order_relaxed)); }
    UpdateError error() const { return UpdateError(error_now.load(std::memory_order_relaxed)); }
    // Service thread: sets a header notice ("Updated to v0.2.0"); the MIDI thread takes it once.
    void note(const std::string& text);
    bool take_note(std::string& text);

private:
    std::mutex lock;
    std::string zip, version, asked_zip, notice;
    uint32_t offers = 0, offers_seen = 0;
    uint8_t asked = 0;
    bool noticed = false;
    std::atomic<uint8_t> phase_now {0}, error_now {0};
};

} // namespace melodee
