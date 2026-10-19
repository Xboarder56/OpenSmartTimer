#pragma once

// Timer identity + current boot id. Both come from the local timer
// state machine (DeviceConfig for timer_id assigned at enrollment,
// TimerController for the volatile boot_id generated at each boot).
// CapabilityToken reads these on every mint and verify.

namespace opensmarttimer { namespace relay {

class ITimerIdentity {
public:
    virtual ~ITimerIdentity() = default;

    // Relay-scoped opaque id, ASCII, NUL-terminated. Not a secret.
    // Empty string when not enrolled.
    virtual const char* timer_id() = 0;

    // Current-boot id as hex, ASCII, NUL-terminated. Matches the
    // boot_id field in local/status.schema.json. Regenerated on every
    // reboot; changes cause all previously minted capability tokens
    // to fail verify (RELAY_PROTOCOL.md §4).
    virtual const char* current_boot_id() = 0;
};

}  // namespace relay
}  // namespace opensmarttimer
