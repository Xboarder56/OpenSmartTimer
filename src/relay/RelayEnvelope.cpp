// RelayEnvelope.cpp — shared constants + tiny helpers.

#include "RelayEnvelope.h"

namespace opensmarttimer { namespace relay {

const char* resultCodeName(ResultCode code) {
    switch (code) {
        case ResultCode::Applied:                 return "applied";
        case ResultCode::InvalidFrame:            return "invalid_frame";
        case ResultCode::Unauthorized:            return "unauthorized";
        case ResultCode::StaleBoot:               return "stale_boot";
        case ResultCode::StaleSession:            return "stale_session";
        case ResultCode::InvalidState:            return "invalid_state";
        case ResultCode::InvalidDuration:         return "invalid_duration";
        case ResultCode::EmergencyInputActive:    return "emergency_input_active";
        case ResultCode::ControlsLocked:          return "controls_locked";
        case ResultCode::MaximumDurationReached:  return "maximum_duration_reached";
        case ResultCode::UnsupportedOpcode:       return "unsupported_opcode";
        case ResultCode::OperationConflict:       return "operation_conflict";
        case ResultCode::SystemTransition:        return "system_transition";
        case ResultCode::Expired:                 return "expired";
        case ResultCode::Throttled:               return "throttled";
        case ResultCode::ControlDisabled:         return "control_disabled";
    }
    return "unknown";
}

}  // namespace relay
}  // namespace opensmarttimer
