#if defined(ESP_PLATFORM)
#include "RelayRuntime.h"

#include <new>
#include <atomic>
#include <string.h>

#include "EspEnroll.h"
#include "EspPlatform.h"
#include "EspWssTransport.h"
#include "RelayEndpoint.h"
#include "RelaySetupApproval.h"
#include "TimerAuthorityControl.h"
#include "TimerController.h"
#include "TlsArena.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace opensmarttimer { namespace relay {

namespace {
constexpr const char* kNsConfig = "relay.cfg";
constexpr const char* kKeyCredentials = "cred";
constexpr const char* kKeyEnabled = "enabled";
// Namespaces CapabilityToken owns; erased on forget and factory reset.
constexpr const char* kTokenNamespaces[] = {"relay.tokens", "relay.capkey", "relay.signkey"};
constexpr uint8_t kCredentialsVersion = 2;
// Large enough for a TLS handshake plus RelayClient's channel buffers.
// The measured low-water mark is reported in status for tuning.
constexpr uint32_t kTaskStackBytes = 16384;
constexpr UBaseType_t kTaskPriority = 1;  // same as the Arduino loop task
constexpr uint32_t kMailboxWaitMs = 3000;
constexpr uint32_t kSnapshotPublishMs = 100;
constexpr uint32_t kWaitReadableMs = 50;
constexpr size_t kWireMax = 520;
constexpr size_t kWireCacheSlots = kTokenTableCapacity;  // a share URL for every live link

struct StoredCredentials {
    uint8_t version;
    uint8_t reserved[3];
    EnrollmentCredentials cred;
    char partner_origin[96];
};

enum class Slot : uint8_t { Idle, Pending, Running, Done };
// Pause: local Bluetooth has priority. The task exits without cleanup or
// storage writes; RelayCore (links, tokens, grant sync) stays in RAM.
enum class StopKind : uint8_t { None, Disable, Forget, Reset, Pause };
constexpr uint32_t kBluetoothResumeDelayMs = 3000;
constexpr uint32_t kResumeRetryMs = 5000;

uint32_t nowMs() { return uint32_t(esp_timer_get_time() / 1000); }

bool validCredentials(const StoredCredentials& stored, size_t len) {
    RelayEndpoint endpoint{};
    const bool legacy = stored.version == 1 && len == offsetof(StoredCredentials, partner_origin);
    const bool current = stored.version == kCredentialsVersion && len == sizeof(stored);
    RelayEndpoint partner{};
    return (legacy || (current && memchr(stored.partner_origin, 0, sizeof(stored.partner_origin))
                      && parseHttpsOrigin(stored.partner_origin, &partner)))
        && memchr(stored.cred.timer_id, 0, sizeof(stored.cred.timer_id)) && stored.cred.timer_id[0]
        && memchr(stored.cred.wss_endpoint, 0, sizeof(stored.cred.wss_endpoint))
        && parseWssUrl(stored.cred.wss_endpoint, &endpoint)
        && (legacy || !sameOrigin(endpoint, partner));
}

RelayRuntime* g_runtime = nullptr;
RelayRuntime::Impl* g_impl = nullptr;
}  // namespace

// Task-side ITimerControl: snapshots come from the copy the loop publishes;
// mutations are handed to the loop, which applies them with the shared
// TimerCommandAuthority and answers through the mailbox.
class LoopTimerProxy : public ITimerControl {
public:
    explicit LoopTimerProxy(RelayRuntime::Impl& impl) : impl_(impl) {}
    void snapshot(StatusSnapshot* out) override;
    MutationOutcome apply(const MutationRequest& req) override;
private:
    RelayRuntime::Impl& impl_;
};

struct RelayCore {
    EspClock clock;
    EspEntropy rng;
    RelayIdentity identity;
    EspWssTransport transport;
    CapabilityToken tokens;
    TimerAuthorityControl control;   // loop task only
    LoopTimerProxy proxy;
    RelayClient client;
    struct Wire { char fp[24]; char token[kWireMax]; } wires[kWireCacheSlots] = {};
    RelayLinkInfo links[kTokenTableCapacity] = {};
    size_t link_count = 0;
    char wss_endpoint[128] = {};
    bool tokens_ready = false;
    uint32_t last_live_count_ms = 0;

    RelayCore(RelayRuntime::Impl& impl, INvsStore& nvs, TimerController& timer, const char* build_id)
        : tokens({&nvs, &clock, &rng, &identity}),
          control(timer, clock, build_id),
          proxy(impl),
          client({&proxy, &tokens, &clock, &transport}) {}
};

struct RelayRuntime::Impl {
    TimerController& timer;
    const char* build_id;
    const char* hardware_model;
    const char* chip_family;
    EspNvs nvs;  // ESP-IDF NVS is safe to use from both tasks
    portMUX_TYPE spin = portMUX_INITIALIZER_UNLOCKED;

    // Loop-owned.
    uint32_t boot_id = 0;
    char device_label[33] = {};
    uint32_t last_event_revision = 0;
    uint32_t last_publish_ms = 0;
    void (*identify_handler)(void*) = nullptr;
    void* identify_context = nullptr;

    // Allocated on the first start in a boot and kept until restart, so
    // revocations queued by disable() reach the relay on the next enable.
    RelayCore* core = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<bool> running{false};
    std::atomic<StopKind> stop{StopKind::None};
    std::atomic<bool> maintenance{false};
    std::atomic<bool> authorized{false};
    bool network_up = false;          // last values from loop(), applied on start
    bool mutations_allowed = false;
    std::atomic<bool> enabled_cache{false};
    std::atomic<bool> enrolled_cache{false};
    std::atomic<bool> origin_approved_cache{false};
    char partner_origin[96] = {};
    RelaySetupApproval setup_approval; // loop-owned; never changed by relay frames
    std::atomic<RelaySetupState> setup_state{RelaySetupState::Idle};
    StopKind completed_stop = StopKind::None; // read only after running=false
    RelayOpResult stop_result = RelayOpResult::Ok;

    // Mailbox: relay task -> loop.
    volatile Slot mailbox_state = Slot::Idle;
    const MutationRequest* mailbox_request = nullptr;
    MutationOutcome mailbox_outcome{};

    // Snapshot and events: loop -> relay task.
    StatusSnapshot published{};
    uint32_t pending_event = 0;

    // Jobs: loop -> relay task.
    RelayOwnerJob job;
    char job_fp[24] = {};
    RelayOpResult job_result = RelayOpResult::Ok;
    // Later hold limit for the session a just-extended link holds; applied by
    // the loop (only it may touch TimerController). 0 = none.
    std::atomic<uint32_t> pending_hold_limit{0};
    // The one link being revoked: refused by the loop from acceptance until
    // the relay task has marked it revoked in the token table. Other links
    // keep working. Only one owner job runs at a time.
    char denied_fp[24] = {};
    // A revocation accepted during a session is saved once the timer is open.
    std::atomic<bool> table_unsaved{false};
    // Set by a failed revocation save: every link stays refused until an
    // explicit enable (a Bluetooth pause/resume never lifts it).
    std::atomic<bool> storage_deny{false};
    // Bluetooth pause/resume (loop-owned except `resuming`, read by the task).
    std::atomic<bool> bluetooth_active{false};
    std::atomic<bool> resuming{false};
    uint32_t bluetooth_clear_ms = 0;
    uint32_t resume_retry_ms = 0;
    bool emergency_seen = false;  // loop-owned

    // Enrollment request: loop -> relay task (asynchronous).
    std::atomic<bool> enroll_pending{false};
    std::atomic<bool> enrolling{false};
    char enroll_origin[80] = {};
    char enroll_code[129] = {};
    uint8_t enroll_pin[kSpkiPinBytes] = {};
    char enroll_partner_origin[96] = {};

    // Status: relay task -> loop.
    RelayStatusInfo published_status{};
    char last_error[40] = {};

    Impl(TimerController& t, const char* b, const char* h, const char* c)
        : timer(t), build_id(b), hardware_model(h), chip_family(c) {
        nvs.setWriteGuard([](void* context) {
            return static_cast<Impl*>(context)->maintenance.load();
        }, this);
    }

    // ---- persistence (either task) ----
    bool loadCredentials(StoredCredentials* out) {
        size_t len = sizeof(*out);
        return nvs.readBlob(kNsConfig, kKeyCredentials, reinterpret_cast<uint8_t*>(out), &len)
            && validCredentials(*out, len);
    }
    bool loadEnabled() {
        uint8_t value = 0; size_t len = 1;
        return nvs.readBlob(kNsConfig, kKeyEnabled, &value, &len) && len == 1 && value == 1;
    }
    bool storeEnabled(bool enabled) {
        const uint8_t value = enabled ? 1 : 0;
        const bool ok = nvs.writeBlob(kNsConfig, kKeyEnabled, &value, 1);
        if (ok) enabled_cache = enabled;
        return ok;
    }
    void refreshCaches() {
        StoredCredentials stored{};
        enrolled_cache = loadCredentials(&stored);
        origin_approved_cache = enrolled_cache && stored.version == kCredentialsVersion;
        portENTER_CRITICAL(&spin);
        memset(partner_origin, 0, sizeof(partner_origin));
        if (origin_approved_cache) memcpy(partner_origin, stored.partner_origin, sizeof(partner_origin));
        portEXIT_CRITICAL(&spin);
        enabled_cache = loadEnabled();
    }
    void setError(const char* text) {
        portENTER_CRITICAL(&spin);
        strncpy(last_error, text ? text : "", sizeof(last_error) - 1);
        last_error[sizeof(last_error) - 1] = '\0';
        portEXIT_CRITICAL(&spin);
    }

    // ---- loop side ----
    void publishSnapshot() {
        StatusSnapshot snapshot{};
        core->control.snapshot(&snapshot);
        portENTER_CRITICAL(&spin);
        published = snapshot;
        portEXIT_CRITICAL(&spin);
        last_publish_ms = nowMs();
    }

    void serviceMailbox() {
        portENTER_CRITICAL(&spin);
        if (mailbox_state != Slot::Pending) { portEXIT_CRITICAL(&spin); return; }
        mailbox_state = Slot::Running;
        const MutationRequest* request = mailbox_request;
        const bool denied = denied_fp[0] && request->token_fp_hash &&
            strcmp(denied_fp, request->token_fp_hash) == 0;
        portEXIT_CRITICAL(&spin);
        // Disable/reset/revoke fences queued work immediately, even if the
        // network task is still inside a TLS call. Never touch token storage
        // concurrently from this task.
        const MutationOutcome outcome =
            denied ? MutationOutcome{ResultCode::Unauthorized, false, false}
            : authorized && stop == StopKind::None && !maintenance
            ? core->control.apply(*request)
            : MutationOutcome{ResultCode::SystemTransition, false, false};
        publishSnapshot();  // the result frame must carry post-mutation state
        portENTER_CRITICAL(&spin);
        mailbox_outcome = outcome;
        mailbox_state = Slot::Done;
        portEXIT_CRITICAL(&spin);
    }

    // resume=true restarts a Bluetooth-paused relay: allowed during a session,
    // no storage lease, and the in-RAM token table is kept (never reloaded,
    // which could revive a revocation not yet saved).
    RelayOpResult start(bool resume = false) {
        if (running) return RelayOpResult::Busy;
        if (!resume && !timer.isOpen()) return RelayOpResult::TimerActive;
        if (!core) {
            if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < sizeof(RelayCore) + 1024)
                return RelayOpResult::Unavailable;
            core = new (std::nothrow) RelayCore(*this, nvs, timer, build_id);
            if (!core) return RelayOpResult::Unavailable;
            core->control.begin(boot_id);
            core->control.setDeviceLabel(device_label);
            core->control.setIdentifyHandler(identify_handler, identify_context);
            core->identity.setBootId(boot_id);
            publishSnapshot();
        }
        if (!resume) {
            maintenance = true; // starts/config writes are interlocked until init completes
            storage_deny = false;
        }
        resuming = resume;
        setError("");
        // Apply the loop's latest gates before the task's first pass.
        core->transport.setAllowed(network_up && mutations_allowed);
        core->control.setMutationsAllowed(mutations_allowed);
        // At boot this runs before Bluetooth starts, while the heap is still
        // whole. Without the block the relay uses the general heap as before.
        TlsArena::reserve(kTlsArenaBytes);
        running = true;
        if (xTaskCreate(&Impl::taskEntry, "relay", kTaskStackBytes, this, kTaskPriority, &task) != pdPASS) {
            running = false;
            task = nullptr;
            maintenance = false;
            return RelayOpResult::Unavailable;
        }
        return RelayOpResult::Ok;
    }

    // Nonblocking submission; arguments become immutable before publication.
    RelayOpResult submit(const RelayOwnerJob::Request& request) {
        const bool paused = stop == StopKind::Pause;
        if (!running && !paused) return RelayOpResult::NotRunning;
        portENTER_CRITICAL(&spin);
        const auto checked = job.check(request);
        RelayOpResult result = RelayOpResult::Accepted;
        if (checked == RelayOwnerJob::Check::Replay)
            result = job.state == RelayOwnerJob::State::Done ? job_result : RelayOpResult::Accepted;
        else if (checked == RelayOwnerJob::Check::Conflict) result = RelayOpResult::Conflict;
        else if (checked == RelayOwnerJob::Check::Busy || maintenance || (stop != StopKind::None && !paused))
            result = RelayOpResult::Busy;
        else if (paused && request.kind == RelayOwnerJob::Kind::Mint)
            result = RelayOpResult::Paused;  // a new link needs the live relay
        else if (!timer.isOpen() && request.kind == RelayOwnerJob::Kind::Mint)
            result = RelayOpResult::TimerActive;
        else {
            // Revoke is allowed while active and refuses only that link, at
            // once. Its flash write waits for an open-timer lease; a mint runs
            // only under that lease.
            // While paused, a revoke waits for the resume, which runs it
            // before reconnecting.
            if (request.kind == RelayOwnerJob::Kind::Revoke) strcpy(denied_fp, request.argument);
            else if (request.kind == RelayOwnerJob::Kind::Mint) authorized = false;
            maintenance = timer.isOpen() && !paused;
            job_fp[0] = 0;
            job_result = RelayOpResult::Accepted;
            job.accept(request);
        }
        portEXIT_CRITICAL(&spin);
        return result;
    }

    // ---- relay task ----
    static void taskEntry(void* arg) { static_cast<Impl*>(arg)->run(); }

    void run() {
        StoredCredentials stored{};
        if (loadCredentials(&stored)) {
            core->identity.setTimerId(stored.cred.timer_id);
            strncpy(core->wss_endpoint, stored.cred.wss_endpoint, sizeof(core->wss_endpoint) - 1);
        }
        if (stop == StopKind::None) {
            if (!resuming || !core->tokens_ready) {
                core->tokens_ready = core->tokens.begin();
                if (!core->tokens_ready) setError("key_storage_failed");
            }
            core->client.begin();
            authorized = core->tokens_ready && enabled_cache && origin_approved_cache && !storage_deny;
            if (!enroll_pending && !resuming) maintenance = false;
            resuming = false;
        }
        while (stop == StopKind::None) {
            processEnrollment();
            processJob();
            saveChanges();
            if (stop != StopKind::None) break;
            if (core->transport.connectPending()) core->transport.performConnect();
            if (!core->transport.allowed() && core->transport.state() == WssState::Open)
                core->transport.close(1001, "paused");
            core->transport.waitReadable(kWaitReadableMs);
            portENTER_CRITICAL(&spin);
            const uint32_t event = pending_event;
            pending_event = 0;
            portEXIT_CRITICAL(&spin);
            if (core->tokens_ready && stop == StopKind::None) {
                if (event) { core->client.onEventEmitted(event); core->client.onStatusChanged(); }
                core->client.loop();
            }
            publishStatus();
        }
        if (stop == StopKind::Pause) {
            // Bluetooth pause: end the session and free this task's stack.
            // Nothing is written; links and grant state stay in RelayCore. A
            // disable/forget/reset requested meanwhile runs from loop().
            authorized = false;
            core->transport.close(1001, "local_bluetooth");
            publishStatus();
            completed_stop = StopKind::Pause;
            // Release any storage lease loop() granted meanwhile (a pending
            // job or unsaved table): with no task, nothing else would, and a
            // held lease refuses every local start. loop() grants it again
            // after resume if the work is still pending.
            maintenance = false;
            task = nullptr;
            running = false;
            vTaskDelete(nullptr);
        }
        authorized = false;
        core->transport.close(4010, "emergency_revoke");
        // Only this task owns relay persistence. Reset never erases underneath
        // an enrollment/job writer. Wait without blocking the Arduino loop.
        for (;;) {
            while (!maintenance) vTaskDelay(pdMS_TO_TICKS(10));
            const StopKind requested = stop.load();
            bool ok = true;
            if (requested == StopKind::Reset || requested == StopKind::Forget) {
                ok = nvs.eraseNamespace(kNsConfig);
                for (const char* ns : kTokenNamespaces) ok = nvs.eraseNamespace(ns) && ok;
                // Clear RAM only after attempting ALL namespaces; errors stay
                // visible and a retry will erase again rather than assume absence.
                core->tokens_ready = false;
                core->client.forgetGrants();
                refreshCaches();
            } else {
                if (core->tokens_ready) {
                    core->client.emergencyShutdown();
                    ok = core->client.lastPersistenceSucceeded();
                } else ok = storeEnabled(false);
            }
            wipeWires();
            portENTER_CRITICAL(&spin);
            if (job.busy()) { job_result = RelayOpResult::NotRunning; job.state = RelayOwnerJob::State::Done; }
            portEXIT_CRITICAL(&spin);
            if (!ok) setError("relay_cleanup_storage_failed");
            // A factory reset can supersede disable while its writes finish.
            if (stop != requested) continue;
            stop_result = ok ? RelayOpResult::Ok : RelayOpResult::StorageFailed;
            completed_stop = requested;
            break;
        }
        enroll_pending = false;
        enrolling = false;
        memset(enroll_code, 0, sizeof(enroll_code));
        publishStatus();
        if (completed_stop != StopKind::Reset) maintenance = false;
        // Last touch of shared state: after this a new task may start.
        task = nullptr;
        running = false;
        vTaskDelete(nullptr);
    }

    void processEnrollment() {
        if (!enroll_pending) return;
        enrolling = true;
        enroll_pending = false;
        core->transport.close(1000, "enroll"); // never allocate two TLS sessions
        if (!core->transport.allowed() ||
            heapFree() < kTlsHeapFloorFreeBytes || heapLargestFreeBlock() < kTlsHeapFloorLargestBlockBytes) {
            setError("network_unavailable");
            setup_state = RelaySetupState::Failed;
            enrolling = false;
            maintenance = false;
            authorized = enabled_cache && core->tokens_ready && origin_approved_cache;
            return;
        }
        EnrollmentCredentials cred{};
        int http_status = 0;
        StatusSnapshot snapshot{};
        core->proxy.snapshot(&snapshot);
        const EnrollOutcome outcome = enrollOverHttps(
            enroll_origin, enroll_code, enroll_pin, hardware_model, chip_family,
            build_id, &cred, &http_status, snapshot.device_label);
        memset(enroll_code, 0, sizeof(enroll_code));
        if (stop != StopKind::None) { enrolling = false; return; }
        if (outcome != EnrollOutcome::Ok) {
            setup_state = RelaySetupState::Failed;
            setError(enrollOutcomeName(outcome));
            enrolling = false;
            // A task started only to enroll releases its stack on failure.
            maintenance = false;
            authorized = enabled_cache && core->tokens_ready && origin_approved_cache;
            return;
        }
        // End old grants BEFORE changing identity. A failed reenrollment must
        // not leave old links attached to partially updated credentials.
        RelayEndpoint returned{}, partner{};
        if (!parseWssUrl(cred.wss_endpoint, &returned)
            || !parseHttpsOrigin(enroll_partner_origin, &partner) || sameOrigin(returned, partner)) {
            setError("invalid_partner_origin");
            setup_state = RelaySetupState::Failed;
            enrolling = false;
            maintenance = false;
            authorized = enabled_cache && core->tokens_ready && origin_approved_cache;
            return;
        }
        if (!core->tokens.wipeAllTokens() || !core->client.saveEnrollmentCredentials(cred)) {
            setError("credential_storage_failed");
            setup_state = RelaySetupState::Failed;
            enrolling = false;
            stop = StopKind::Disable;
            return;
        }
        // Links minted for an earlier enrollment belong to another relay identity.
        core->client.forgetGrants();
        wipeWires();
        core->identity.setTimerId(cred.timer_id);
        portENTER_CRITICAL(&spin);
        strncpy(core->wss_endpoint, cred.wss_endpoint, sizeof(core->wss_endpoint) - 1);
        portEXIT_CRITICAL(&spin);
        setError("");
        enrolling = false;
        setup_state = stop == StopKind::None ? RelaySetupState::Complete : RelaySetupState::Cancelled;
        authorized = core->tokens_ready && origin_approved_cache && stop == StopKind::None;
        if (stop == StopKind::None) maintenance = false;
    }

    void processJob() {
        portENTER_CRITICAL(&spin);
        // A revoke needs no flash write to take effect, so it never waits
        // for the storage lease; a mint does.
        if (job.state != RelayOwnerJob::State::Pending || stop != StopKind::None ||
            (!maintenance && job.request.kind == RelayOwnerJob::Kind::Mint)) {
            portEXIT_CRITICAL(&spin); return;
        }
        job.state = RelayOwnerJob::State::Running;
        const RelayOwnerJob::Request request = job.request;
        portEXIT_CRITICAL(&spin);
        const RelayOpResult result = executeJob(request);
        // Refresh the owner-visible link list before reporting the job done,
        // so a caller that saw "done" never reads a stale list.
        core->last_live_count_ms = nowMs() - 1000;
        publishStatus();
        portENTER_CRITICAL(&spin);
        job_result = result;
        job.state = RelayOwnerJob::State::Done;
        denied_fp[0] = '\0';  // the token table now refuses it (or it never existed)
        // On a storage failure, retain the deny gate until explicit disable
        // and enable. Never resume access with ambiguous persisted revocation.
        if (result == RelayOpResult::StorageFailed) storage_deny = true;
        if (request.kind == RelayOwnerJob::Kind::Mint)
            authorized = !storage_deny && enabled_cache && origin_approved_cache && stop == StopKind::None;
        else if (result == RelayOpResult::StorageFailed)
            authorized = false;
        if (stop == StopKind::None) maintenance = false;
        portEXIT_CRITICAL(&spin);
    }

    RelayOpResult executeJob(const RelayOwnerJob::Request& request) {
        RelayClient& client = core->client;
        CapabilityToken& tokens = core->tokens;
        switch (request.kind) {
            case RelayOwnerJob::Kind::Mint: return mintLink(request);
            case RelayOwnerJob::Kind::Revoke: {
                if (!core->tokens_ready) return RelayOpResult::NotRunning;
                // During a session there is no storage lease: revoke in RAM
                // (tokens are boot-scoped) and save when the timer is open.
                const bool persist = maintenance.load();
                const RevokeResult revoked = tokens.revokeByHash(request.argument, persist);
                if (revoked == RevokeResult::NotFound) return RelayOpResult::NotFound;
                if (!persist && revoked == RevokeResult::Ok) table_unsaved = true;
                client.announceRevokedToken(request.argument);
                portENTER_CRITICAL(&spin);
                strcpy(job_fp, request.argument);
                for (auto& wire : core->wires) if (!strcmp(wire.fp, request.argument)) memset(&wire, 0, sizeof(wire));
                portEXIT_CRITICAL(&spin);
                return revoked == RevokeResult::Ok ? RelayOpResult::Ok : RelayOpResult::StorageFailed;
            }
            case RelayOwnerJob::Kind::Extend: {
                if (!core->tokens_ready) return RelayOpResult::NotRunning;
                // Like revoke: no flash lease needed; saved when open.
                const bool persist = maintenance.load();
                uint32_t expires = 0;
                const RevokeResult extended = tokens.extendByHash(request.argument, request.ttl_ms,
                                                                  persist, &expires);
                if (extended == RevokeResult::NotFound) return RelayOpResult::NotFound;
                if (!persist && extended == RevokeResult::Ok) table_unsaved = true;
                if (!client.announceExtendedToken(request.argument, expires)) {
                    // Grant sync slot reused or never created: announce anew
                    // from the cached wire token (same URL, new expiry).
                    portENTER_CRITICAL(&spin);
                    char wire[kWireMax] = {};
                    for (const auto& entry : core->wires)
                        if (!strcmp(entry.fp, request.argument)) strcpy(wire, entry.token);
                    portEXIT_CRITICAL(&spin);
                    if (wire[0]) client.announceMintedToken(wire, strlen(wire));
                    memset(wire, 0, sizeof(wire));
                }
                pending_hold_limit = expires;
                portENTER_CRITICAL(&spin);
                strcpy(job_fp, request.argument);
                portEXIT_CRITICAL(&spin);
                return extended == RevokeResult::Ok ? RelayOpResult::Ok : RelayOpResult::StorageFailed;
            }
        }
        return RelayOpResult::InvalidRequest;
    }

    void saveChanges() {
        portENTER_CRITICAL(&spin);
        const bool ready = table_unsaved && maintenance && !job.busy() && stop == StopKind::None;
        portEXIT_CRITICAL(&spin);
        if (!ready) return;
        if (!core->tokens.saveChanges()) {
            // Same policy as a failed revoke: refuse every link until an
            // explicit disable/enable (which rewrites the table) instead of
            // holding the timer in maintenance to retry.
            setError("key_storage_failed");
            storage_deny = true;
            authorized = false;
        }
        table_unsaved = false;
        if (stop == StopKind::None) maintenance = false;
    }

    RelayOpResult mintLink(const RelayOwnerJob::Request& request) {
        if (!core->tokens_ready) return RelayOpResult::NotRunning;
        if (!core->client.sharingAvailable()) return RelayOpResult::NotConnected;
        if (!core->client.canQueueGrant()) return RelayOpResult::TableFull;
        char wire[kWireMax];
        size_t wire_len = 0;
        switch (core->tokens.mint(kPartnerScopeMask, request.ttl_ms, request.argument, wire, sizeof(wire), &wire_len)) {
            case MintResult::Ok: break;
            case MintResult::TableFull: return RelayOpResult::TableFull;
            case MintResult::RateLimited: return RelayOpResult::RateLimited;
            case MintResult::InvalidRequest: return RelayOpResult::InvalidRequest;
            case MintResult::NvsWriteFailed: return RelayOpResult::StorageFailed;
        }
        TokenRecord* record = nullptr;
        if (core->tokens.verify(wire, wire_len, Scope::View, &record) != VerifyResult::Ok || !record)
            return RelayOpResult::StorageFailed;
        char fp[24];
        if (!CapabilityToken::fingerprintHash(record->fp, fp)) return RelayOpResult::StorageFailed;
        if (!core->client.announceMintedToken(wire, wire_len)) {
            if (core->tokens.revokeByHash(fp) != RevokeResult::Ok) return RelayOpResult::StorageFailed;
            return RelayOpResult::TableFull;
        }
        // Keep every live link's share URL: reuse an empty slot or one whose
        // link was revoked or expired, never another live link's.
        TokenRecord records[kTokenTableCapacity];
        char live[kTokenTableCapacity][24] = {};
        size_t live_count = core->tokens.listLive(records, kTokenTableCapacity);
        for (size_t i = 0; i < live_count; ++i)
            if (!CapabilityToken::fingerprintHash(records[i].fp, live[i])) live[i][0] = '\0';
        portENTER_CRITICAL(&spin);
        strcpy(job_fp, fp);
        const size_t index = chooseShareSlot(core->wires, live, live_count);
        if (index < kWireCacheSlots) {
            RelayCore::Wire& slot = core->wires[index];
            strcpy(slot.fp, fp);
            memcpy(slot.token, wire, wire_len);
            slot.token[wire_len] = '\0';
        }
        portEXIT_CRITICAL(&spin);
        memset(wire, 0, sizeof(wire));
        return RelayOpResult::Ok;
    }

    void wipeWires() {
        portENTER_CRITICAL(&spin);
        memset(core->wires, 0, sizeof(core->wires));
        core->link_count = 0;
        portEXIT_CRITICAL(&spin);
    }

    void publishStatus() {
        RelayStatusInfo info{};
        const RelayDiagnostics diagnostics = core->client.diagnostics();
        info.state = diagnostics.state;
        info.reconnects = diagnostics.reconnect_count;
        info.sharing_available = authorized && stop == StopKind::None && !maintenance &&
            core->tokens_ready && core->client.sharingAvailable();
        const uint32_t now = nowMs();
        // Disable, Forget and Reset end every link from the moment they are
        // requested. The table is recounted at most once a second and the
        // task exits after its last publish, so without this the count from
        // before the stop stayed in the status until sharing was enabled again.
        const StopKind stopping = stop.load();
        if (stopping != StopKind::None && stopping != StopKind::Pause) {
            info.live_links = 0;
        } else if (core->tokens_ready && now - core->last_live_count_ms >= 1000) {
            TokenRecord records[kTokenTableCapacity];
            core->last_live_count_ms = now;
            info.live_links = uint32_t(core->tokens.listLive(records, kTokenTableCapacity));
            // Publish bounded read-only snapshots; local GETs never wait for TLS.
            for (size_t i = 0; i < info.live_links; ++i) {
                RelayLinkInfo link{};
                if (!CapabilityToken::fingerprintHash(records[i].fp, link.fp_hash)) continue;
                memcpy(link.label, records[i].label, sizeof(link.label));
                link.label[sizeof(link.label) - 1] = 0;
                link.expires_in_ms = records[i].exp_up_ms; // absolute in cache
                link.state = core->client.grantState(link.fp_hash);
                for (const auto& wire : core->wires)
                    if (!strcmp(wire.fp, link.fp_hash)) link.url_available = true;
                portENTER_CRITICAL(&spin);
                core->links[i] = link;
                portEXIT_CRITICAL(&spin);
            }
            portENTER_CRITICAL(&spin);
            core->link_count = info.live_links;
            portEXIT_CRITICAL(&spin);
        } else {
            info.live_links = published_status.live_links;
        }
        RelayEndpoint endpoint{};
        if (parseWssUrl(core->wss_endpoint, &endpoint)) strcpy(info.relay_host, endpoint.host);
        strncpy(info.timer_id, core->identity.timer_id(), sizeof(info.timer_id) - 1);
        info.task_stack_min_free_bytes = task ? uint32_t(uxTaskGetStackHighWaterMark(nullptr)) : 0;
        const bool retrying = diagnostics.state == SessionState::Backoff
            || diagnostics.state == SessionState::Failed;
        // Prefer the transport's specific error (TLS, pin); otherwise report
        // the session-level reason, e.g. a handshake timeout.
        const char* reason = core->transport.lastError() != WssError::None
            ? core->transport.lastErrorName() : diagnostics.last_failure;
        // The client is not attempting a connection at all while memory is
        // short, so that current reason replaces an older connection error.
        const bool low_memory = diagnostics.state == SessionState::Backoff && diagnostics.last_failure
            && strcmp(diagnostics.last_failure, "low_memory") == 0;
        portENTER_CRITICAL(&spin);
        if (low_memory) {
            strncpy(last_error, "low_memory", sizeof(last_error) - 1);
        } else if (retrying && reason && reason[0] && !last_error[0]) {
            strncpy(last_error, reason, sizeof(last_error) - 1);
        } else if (diagnostics.state == SessionState::Live && stop == StopKind::None &&
                   setup_state != RelaySetupState::Failed &&
                   strcmp(last_error, "key_storage_failed") != 0) {
            // A live session clears stale connection errors, but never the
            // reason for a stop or failed setup. Reconnecting to the previous
            // relay must preserve the new enrollment's failure for the owner.
            last_error[0] = '\0';
        }
        memcpy(info.last_error, last_error, sizeof(info.last_error));
        published_status = info;
        portEXIT_CRITICAL(&spin);
    }
};

void LoopTimerProxy::snapshot(StatusSnapshot* out) {
    portENTER_CRITICAL(&impl_.spin);
    *out = impl_.published;
    portEXIT_CRITICAL(&impl_.spin);
}

MutationOutcome LoopTimerProxy::apply(const MutationRequest& req) {
    portENTER_CRITICAL(&impl_.spin);
    impl_.mailbox_request = &req;
    impl_.mailbox_state = Slot::Pending;
    portEXIT_CRITICAL(&impl_.spin);
    const uint32_t started = nowMs();
    for (;;) {
        vTaskDelay(1);
        portENTER_CRITICAL(&impl_.spin);
        if (impl_.mailbox_state == Slot::Done) {
            const MutationOutcome outcome = impl_.mailbox_outcome;
            impl_.mailbox_state = Slot::Idle;
            portEXIT_CRITICAL(&impl_.spin);
            return outcome;
        }
        // Withdraw only a request the loop has not started; one in progress
        // is always waited for, so the reported outcome is never a guess.
        if (impl_.mailbox_state == Slot::Pending && nowMs() - started >= kMailboxWaitMs) {
            impl_.mailbox_state = Slot::Idle;
            portEXIT_CRITICAL(&impl_.spin);
            return {ResultCode::SystemTransition, false, false};
        }
        portEXIT_CRITICAL(&impl_.spin);
    }
}

// Persistence and platform hooks declared weak in RelayClient.cpp.
bool RelayClient::readEnrollmentCredentialsFromNvs() {
    StoredCredentials stored{};
    if (!g_impl || !g_impl->loadCredentials(&stored)) return false;
    cred_ = stored.cred;
    return true;
}
bool RelayClient::writeEnrollmentCredentialsToNvs() {
    if (!g_impl) return false;
    StoredCredentials stored{};
    stored.version = kCredentialsVersion;
    stored.cred = cred_;
    const char* approved = g_impl->enrolling ? g_impl->enroll_partner_origin : g_impl->partner_origin;
    RelayEndpoint partner{}, relay{};
    if (!parseHttpsOrigin(approved, &partner) || !parseWssUrl(cred_.wss_endpoint, &relay)
        || sameOrigin(partner, relay)) return false;
    strcpy(stored.partner_origin, approved);
    const bool ok = g_impl->nvs.writeBlob(kNsConfig, kKeyCredentials,
                                          reinterpret_cast<const uint8_t*>(&stored), sizeof(stored));
    if (ok) {
        g_impl->enrolled_cache = true;
        g_impl->origin_approved_cache = true;
        portENTER_CRITICAL(&g_impl->spin);
        strcpy(g_impl->partner_origin, stored.partner_origin);
        portEXIT_CRITICAL(&g_impl->spin);
    }
    return ok;
}
bool RelayClient::eraseEnrollmentCredentialsFromNvs() {
    if (!g_impl) return false;
    const bool ok = g_impl->nvs.eraseKey(kNsConfig, kKeyCredentials);
    if (ok) {
        g_impl->enrolled_cache = false;
        g_impl->origin_approved_cache = false;
        portENTER_CRITICAL(&g_impl->spin);
        g_impl->partner_origin[0] = 0;
        portEXIT_CRITICAL(&g_impl->spin);
    }
    return ok;
}
bool RelayClient::readEnabledPreferenceFromNvs() { return g_impl && g_impl->loadEnabled(); }
bool RelayClient::setEnabledPreferencePersisted(bool enabled) { return g_impl && g_impl->storeEnabled(enabled); }

RelayRuntime::RelayRuntime(TimerController& timer, const char* build_id, const char* hardware_model,
                           const char* chip_family)
    : impl_(new Impl(timer, build_id, hardware_model, chip_family)) {
    g_runtime = this;
    g_impl = impl_;
}

RelayRuntime* RelayRuntime::active() { return g_runtime; }

void RelayRuntime::begin(uint32_t boot_id, const char* device_label) {
    impl_->boot_id = boot_id;
    strncpy(impl_->device_label, device_label ? device_label : "", sizeof(impl_->device_label) - 1);
    impl_->last_event_revision = impl_->timer.eventRevision();
    impl_->refreshCaches();
    if (impl_->enabled_cache && impl_->enrolled_cache && impl_->origin_approved_cache
        && impl_->start() != RelayOpResult::Ok)
        impl_->setError("start_failed_low_memory");
}

void RelayRuntime::loop(bool network_up, bool mutations_allowed, bool bluetooth_active) {
    Impl& impl = *impl_;
    impl.network_up = network_up;
    impl.mutations_allowed = mutations_allowed;
    impl.bluetooth_active = bluetooth_active;
    const uint32_t now = nowMs();
    if (impl.setup_state == RelaySetupState::Pending && !impl.setup_approval.pending(now))
        impl.setup_state = RelaySetupState::Expired;
    impl.setup_approval.expire(now);
    if (!impl.timer.isOpen() || impl.timer.emergencyInputActive() || !mutations_allowed || bluetooth_active) {
        if (impl.setup_state == RelaySetupState::Pending) impl.setup_state = RelaySetupState::Cancelled;
        impl.setup_approval.clear();
    }
    if (bluetooth_active) impl.bluetooth_clear_ms = 0;
    else if (!impl.bluetooth_clear_ms) impl.bluetooth_clear_ms = now | 1;
    if (!impl.core) return;  // never started this boot: no further cost
    // An owner Extend moves the fail-open deadline of a session the link
    // holds (only a session already limited by a link; never adds a limit).
    if (const uint32_t until = impl.pending_hold_limit.exchange(0)) {
        if (impl.timer.snapshot(now).holdLimited) impl.timer.limitHoldUntil(until);
    }
    // The physical emergency input cancels remote sharing: the link ends and
    // the relay turns off until the owner enables it again, so a partner
    // cannot simply restart the timer after an emergency release.
    const bool emergency = impl.timer.emergencyInputActive();
    if (emergency && !impl.emergency_seen && impl.enabled_cache &&
        impl.stop != StopKind::Disable && impl.stop != StopKind::Forget && impl.stop != StopKind::Reset) {
        disable();
        impl.setError("emergency_input");
    }
    impl.emergency_seen = emergency;
    // Local Bluetooth has priority over the relay (memory): pause it, never
    // in the middle of a storage write or an enrollment.
    if (bluetooth_active && impl.running && impl.stop == StopKind::None && !impl.maintenance &&
        !impl.enroll_pending && !impl.enrolling)
        impl.stop = StopKind::Pause;
    if (!bluetooth_active && impl.stop == StopKind::Pause && !impl.running &&
        now - impl.bluetooth_clear_ms >= kBluetoothResumeDelayMs &&
        (!impl.resume_retry_ms || now - impl.resume_retry_ms >= kResumeRetryMs)) {
        impl.stop = StopKind::None;
        impl.completed_stop = StopKind::None;
        if (impl.start(true) == RelayOpResult::Ok) {
            impl.resume_retry_ms = 0;
        } else {  // no room for the task stack yet; retry, and say why
            impl.stop = StopKind::Pause;
            impl.completed_stop = StopKind::Pause;
            impl.resume_retry_ms = now | 1;
            impl.setError("low_memory");
        }
    }
    // A disable/forget requested while paused runs its cleanup task once
    // the timer is open (it needs the storage lease).
    if (!impl.running && (impl.stop == StopKind::Disable || impl.stop == StopKind::Forget) &&
        impl.completed_stop != impl.stop && impl.timer.isOpen() && mutations_allowed)
        impl.start();
    // Grant deferred cleanup/revoke its storage lease only on the loop task,
    // after observing an open timer. Every start path sees maintenanceActive().
    // A Bluetooth pause writes nothing, so it never takes the lease.
    portENTER_CRITICAL(&impl.spin);
    const bool stop_writes = impl.stop != StopKind::None && impl.stop != StopKind::Pause;
    if ((stop_writes || impl.job.busy() || impl.table_unsaved) &&
        impl.running && impl.timer.isOpen() && mutations_allowed) impl.maintenance = true;
    portEXIT_CRITICAL(&impl.spin);
    impl.core->control.setMutationsAllowed(mutations_allowed && !impl.maintenance);
    impl.core->transport.setAllowed(network_up && mutations_allowed && impl.stop == StopKind::None);
    const uint32_t revision = impl.timer.eventRevision();
    if (revision != impl.last_event_revision) {
        impl.last_event_revision = revision;
        impl.publishSnapshot();
        portENTER_CRITICAL(&impl.spin);
        impl.pending_event = revision ? revision : 1;
        portEXIT_CRITICAL(&impl.spin);
    } else if (nowMs() - impl.last_publish_ms >= kSnapshotPublishMs) {
        impl.publishSnapshot();
    }
    impl.serviceMailbox();
}

void RelayRuntime::setIdentifyHandler(void (*handler)(void*), void* context) {
    impl_->identify_handler = handler;
    impl_->identify_context = context;
    if (impl_->core) impl_->core->control.setIdentifyHandler(handler, context);
}

RelayOpResult RelayRuntime::enroll(const char* origin, const char* code,
                                 const uint8_t pin[kSpkiPinBytes], const char* partner_origin) {
    Impl& impl = *impl_;
    RelayEndpoint relay{}, partner{};
    const size_t code_len = code ? strlen(code) : 0;
    if (!origin || !partner_origin || !pin || !parseHttpsOrigin(origin, &relay)
        || strlen(origin) >= sizeof(impl.enroll_origin)
        || !parseHttpsOrigin(partner_origin, &partner)
        || strlen(partner_origin) >= sizeof(impl.enroll_partner_origin)
        || sameOrigin(relay, partner) || code_len < 8 || code_len >= sizeof(impl.enroll_code))
        return RelayOpResult::InvalidRequest;
    if (!impl.timer.isOpen() || impl.timer.emergencyInputActive()) return RelayOpResult::TimerActive;
    if (impl.bluetooth_active || impl.stop == StopKind::Pause) return RelayOpResult::Paused;
    if (!impl.network_up || !impl.mutations_allowed || impl.maintenance || impl.enroll_pending || impl.enrolling)
        return RelayOpResult::Busy;
    if (impl.stop != StopKind::None && (impl.running || impl.completed_stop != impl.stop
        || impl.stop_result != RelayOpResult::Ok || impl.stop == StopKind::Reset)) return RelayOpResult::Busy;
    RelaySetupApproval::Request request{};
    strcpy(request.relay_origin, origin);
    strcpy(request.partner_origin, partner_origin);
    strcpy(request.enrollment_code, code);
    memcpy(request.pin, pin, sizeof(request.pin));
    const auto staged = impl.setup_approval.stage(request, nowMs());
    if (staged != RelaySetupApproval::Stage::Busy) impl.setup_state = RelaySetupState::Pending;
    memset(request.enrollment_code, 0, sizeof(request.enrollment_code));
    return staged == RelaySetupApproval::Stage::Busy ? RelayOpResult::Busy : RelayOpResult::Accepted;
}

bool RelayRuntime::enrollmentApprovalPending() const { return impl_->setup_approval.pending(nowMs()); }

RelayOpResult RelayRuntime::cancelEnrollment(uint32_t id) {
    if (!impl_->setup_approval.cancel(id)) return RelayOpResult::Conflict;
    impl_->setup_state = RelaySetupState::Cancelled;
    return RelayOpResult::Ok;
}

RelayOpResult RelayRuntime::confirmEnrollment(uint32_t id) {
    Impl& impl = *impl_;
    if (impl.setup_approval.id() != id) return RelayOpResult::Conflict;
    RelaySetupApproval::Request request{};
    if (!impl.setup_approval.approve(nowMs(), impl.timer.isOpen() && !impl.timer.emergencyInputActive(),
        impl.mutations_allowed && impl.network_up && !impl.bluetooth_active && !impl.maintenance,
        &request)) {
        impl.setup_state = RelaySetupState::Cancelled;
        return RelayOpResult::Busy;
    }
    impl.setup_state = RelaySetupState::Connecting;
    const auto result = enrollApproved(request.relay_origin, request.enrollment_code,
                                       request.pin, request.partner_origin);
    memset(request.enrollment_code, 0, sizeof(request.enrollment_code));
    if (result != RelayOpResult::Accepted) {
        impl.setup_state = RelaySetupState::Failed;
        impl.setError(resultName(result));
    }
    return result;
}

RelayOpResult RelayRuntime::enrollApproved(const char* origin, const char* code,
                                         const uint8_t pin[kSpkiPinBytes], const char* partner_origin) {
    Impl& impl = *impl_;
    RelayEndpoint endpoint{};
    const size_t code_len = code ? strlen(code) : 0;
    if (!parseHttpsOrigin(origin, &endpoint) || strlen(origin) >= sizeof(impl.enroll_origin)
        || code_len < 8 || code_len >= sizeof(impl.enroll_code) || !pin)
        return RelayOpResult::InvalidRequest;
    const StopKind stop = impl.stop.load();
    // Disable/forget leave their stop marker in place so repeated requests
    // report the completed result. Once cleanup has succeeded and its task
    // has exited, enrollment may start a new task without requiring a reboot.
    const bool stopped_cleanly = !impl.running.load() && impl.completed_stop == stop &&
        (stop == StopKind::Disable || stop == StopKind::Forget) &&
        impl.stop_result == RelayOpResult::Ok;
    if (impl.enroll_pending || impl.enrolling || impl.maintenance ||
        (stop != StopKind::None && !stopped_cleanly))
        return RelayOpResult::Busy;
    if (!impl.timer.isOpen()) return RelayOpResult::TimerActive;
    if (!impl.network_up || !impl.mutations_allowed) return RelayOpResult::Busy;
    if (impl.bluetooth_active || impl.stop == StopKind::Pause) return RelayOpResult::Paused;
    if (stopped_cleanly) {
        impl.stop = StopKind::None;
        impl.completed_stop = StopKind::None;
    }
    strcpy(impl.enroll_origin, origin);
    strcpy(impl.enroll_code, code);
    memcpy(impl.enroll_pin, pin, kSpkiPinBytes);
    strcpy(impl.enroll_partner_origin, partner_origin);
    impl.setError("");
    impl.maintenance = true;
    impl.authorized = false;
    impl.enroll_pending = true;
    const RelayOpResult started = impl.running ? RelayOpResult::Ok : impl.start();
    if (started != RelayOpResult::Ok) {
        impl.enroll_pending = false;
        impl.maintenance = false;
        memset(impl.enroll_code, 0, sizeof(impl.enroll_code));
        return started;
    }
    return RelayOpResult::Accepted;
}

RelayOpResult RelayRuntime::enable() {
    Impl& impl = *impl_;
    if (impl.stop == StopKind::Pause) return RelayOpResult::Ok;  // enabled; resumes after Bluetooth
    if (impl.running || impl.maintenance) return RelayOpResult::Busy;
    if (!impl.timer.isOpen()) return RelayOpResult::TimerActive;
    impl.refreshCaches();
    if (!impl.enrolled_cache) return RelayOpResult::NotEnrolled;
    if (!impl.origin_approved_cache) return RelayOpResult::OriginRequired;
    impl.maintenance = true;
    if (!impl.storeEnabled(true)) { impl.maintenance = false; return RelayOpResult::StorageFailed; }
    impl.stop = StopKind::None;
    impl.completed_stop = StopKind::None;
    return impl.start();
}

RelayOpResult RelayRuntime::disable() {
    Impl& impl = *impl_;
    if (impl.setup_state == RelaySetupState::Pending || impl.setup_state == RelaySetupState::Connecting)
        impl.setup_state = RelaySetupState::Cancelled;
    impl.setup_approval.clear();
    if (impl.stop == StopKind::Reset || impl.stop == StopKind::Forget) return RelayOpResult::Busy;
    if (!impl.running && impl.completed_stop == StopKind::Disable) return impl.stop_result;
    const bool was_paused = impl.stop == StopKind::Pause;
    impl.authorized = false;
    impl.stop = StopKind::Disable;
    if (impl.core) impl.core->transport.setAllowed(false);
    if (!impl.running) {
        // Paused mid-session: remote access is already off; loop() runs the
        // cleanup when the timer opens. Otherwise (failed startup) serialize
        // the persisted preference change through the same cleanup task.
        if (was_paused && !impl.timer.isOpen()) return RelayOpResult::Accepted;
        const RelayOpResult started = impl.start();
        if (started != RelayOpResult::Ok) return started;
    }
    return RelayOpResult::Accepted;
}

RelayOpResult RelayRuntime::forget() {
    Impl& impl = *impl_;
    if (impl.setup_state == RelaySetupState::Pending || impl.setup_state == RelaySetupState::Connecting)
        impl.setup_state = RelaySetupState::Cancelled;
    impl.setup_approval.clear();
    if (!impl.timer.isOpen()) return RelayOpResult::TimerActive;
    if (impl.stop == StopKind::Reset) return RelayOpResult::Busy;
    if (!impl.running && impl.completed_stop == StopKind::Forget) return impl.stop_result;
    impl.authorized = false;
    impl.maintenance = true;
    impl.stop = StopKind::Forget;
    if (impl.core) impl.core->transport.setAllowed(false);
    if (!impl.running) {
        const RelayOpResult started = impl.start();
        if (started != RelayOpResult::Ok) return started;
    }
    return RelayOpResult::Accepted;
}

RelayOpResult RelayRuntime::mint(uint32_t id, uint32_t ttl_ms, const char* label) {
    Impl& impl = *impl_;
    if (impl.setup_approval.pending(nowMs())) return RelayOpResult::Busy;
    if (!ttl_ms || ttl_ms > kMaxLinkTtlMs || (label && strlen(label) > 23))
        return RelayOpResult::InvalidRequest;
    RelayOwnerJob::Request request{};
    request.id = id; request.kind = RelayOwnerJob::Kind::Mint; request.ttl_ms = ttl_ms;
    strcpy(request.argument, label ? label : "");
    return impl.submit(request);
}

RelayOpResult RelayRuntime::revoke(uint32_t id, const char* fp_hash) {
    Impl& impl = *impl_;
    if (!fp_hash || strlen(fp_hash) != 22) return RelayOpResult::InvalidRequest;
    RelayOwnerJob::Request request{};
    request.id = id; request.kind = RelayOwnerJob::Kind::Revoke;
    strcpy(request.argument, fp_hash);
    return impl.submit(request);
}

RelayOpResult RelayRuntime::extend(uint32_t id, const char* fp_hash, uint32_t add_ms) {
    Impl& impl = *impl_;
    if (!fp_hash || strlen(fp_hash) != 22 || !add_ms || add_ms > kMaxLinkTtlMs)
        return RelayOpResult::InvalidRequest;
    RelayOwnerJob::Request request{};
    request.id = id; request.kind = RelayOwnerJob::Kind::Extend; request.ttl_ms = add_ms;
    strcpy(request.argument, fp_hash);
    return impl.submit(request);
}

RelayOpResult RelayRuntime::shareUrl(const char* fp_hash, char* out, size_t cap, GrantState* state) {
    Impl& impl = *impl_;
    if (!fp_hash || strlen(fp_hash) != 22) return RelayOpResult::InvalidRequest;
    if (!impl.core || !impl.authorized || impl.stop != StopKind::None) return RelayOpResult::NotRunning;
    char wire[kWireMax] = {}, endpoint[128] = {}, partner_origin[96] = {};
    GrantState grant = GrantState::Unknown;
    portENTER_CRITICAL(&impl.spin);
    for (size_t i = 0; i < impl.core->link_count; ++i) {
        const auto& link = impl.core->links[i];
        if (!strcmp(link.fp_hash, fp_hash) && link.expires_in_ms > nowMs()) grant = link.state;
    }
    if (grant == GrantState::Ready) for (const auto& entry : impl.core->wires)
        if (!strcmp(entry.fp, fp_hash)) { strcpy(wire, entry.token); break; }
    strcpy(endpoint, impl.core->wss_endpoint);
    strcpy(partner_origin, impl.partner_origin);
    portEXIT_CRITICAL(&impl.spin);
    if (state) *state = grant;
    if (grant != GrantState::Ready) return RelayOpResult::NotReady;
    if (!wire[0]) return RelayOpResult::NotFound;
    const bool ok = shareUrlFromWss(partner_origin, endpoint, fp_hash, wire, out, cap);
    memset(wire, 0, sizeof(wire));
    return ok ? RelayOpResult::Ok : RelayOpResult::InvalidRequest;
}

RelayOpResult RelayRuntime::forEachLink(void (*visit)(const RelayLinkInfo&, void*), void* context) {
    Impl& impl = *impl_;
    if (!impl.core) return RelayOpResult::Ok;
    RelayLinkInfo links[kTokenTableCapacity];
    portENTER_CRITICAL(&impl.spin);
    const size_t count = impl.core->link_count;
    memcpy(links, impl.core->links, count * sizeof(links[0]));
    portEXIT_CRITICAL(&impl.spin);
    const uint32_t now = nowMs();
    const StopKind stop = impl.stop.load();
    const bool ending = stop != StopKind::None && stop != StopKind::Pause;
    for (size_t i = 0; i < count; ++i) {
        if (links[i].expires_in_ms <= now) continue;
        links[i].expires_in_ms -= now;
        if (ending) links[i].state = GrantState::Revoking;
        visit(links[i], context);
    }
    return RelayOpResult::Ok;
}

void RelayRuntime::status(RelayStatusInfo* out) const {
    Impl& impl = *impl_;
    RelayStatusInfo info{};
    portENTER_CRITICAL(&impl.spin);
    if (impl.core) {
        info = impl.published_status;
        memcpy(info.last_error, impl.last_error, sizeof(info.last_error));
    } else {
        info.state = SessionState::Disabled;
        memcpy(info.last_error, impl.last_error, sizeof(info.last_error));
    }
    info.job_id = impl.job.request.id;
    info.next_job_id = impl.job.next_id();
    info.job_state = impl.job.state;
    info.job_result = impl.job_result;
    memcpy(info.job_fp_hash, impl.job_fp, sizeof(info.job_fp_hash));
    portEXIT_CRITICAL(&impl.spin);
    info.boot_id = impl.boot_id;
    info.setup_pending = impl.setup_approval.pending(nowMs());
    info.setup_state = impl.setup_state;
    info.setup_request_id = impl.setup_approval.id();
    info.setup_remaining_ms = impl.setup_approval.remaining(nowMs());
    if (info.setup_pending) {
        strcpy(info.setup_relay_origin, impl.setup_approval.request().relay_origin);
        strcpy(info.setup_partner_origin, impl.setup_approval.request().partner_origin);
    }
    portENTER_CRITICAL(&impl.spin);
    strcpy(info.partner_origin, impl.partner_origin);
    portEXIT_CRITICAL(&impl.spin);
    const StopKind stop = impl.stop.load();
    info.paused_for_bluetooth = stop == StopKind::Pause;
    // completed_stop is read only after running=false (task exit order).
    const bool finished = !impl.running.load() && impl.completed_stop == stop;
    info.stopping = stop != StopKind::None && stop != StopKind::Pause && !finished;
    if (finished && (stop == StopKind::Disable || stop == StopKind::Forget))
        info.state = SessionState::Disabled;
    info.maintenance = impl.maintenance;
    info.enabled = impl.enabled_cache && (stop == StopKind::None || stop == StopKind::Pause);
    if (!impl.running) {
        // The last published session state is stale once the task is gone.
        if (!impl.enrolled_cache) {
            info.state = SessionState::NotEnrolled;
            info.timer_id[0] = '\0';
            info.relay_host[0] = '\0';
        } else {
            info.state = impl.enabled_cache && stop == StopKind::None ? SessionState::Backoff
                                                                        : SessionState::Disabled;
        }
    }
    info.enrolled = impl.enrolled_cache;
    info.running = impl.running;
    info.enrolling = impl.enroll_pending || impl.enrolling;
    if (!info.running || !impl.authorized || impl.maintenance || impl.stop != StopKind::None) {
        info.sharing_available = false;
        info.task_stack_min_free_bytes = impl.published_status.task_stack_min_free_bytes;
    }
    info.free_heap_bytes = uint32_t(heap_caps_get_free_size(MALLOC_CAP_8BIT));
    info.largest_free_block_bytes = uint32_t(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    info.tls_arena_bytes = uint32_t(TlsArena::sizeBytes());
    info.tls_arena_free_bytes = uint32_t(TlsArena::freeBytes());
    info.tls_arena_minimum_free_bytes = uint32_t(TlsArena::minimumFreeBytes());
    info.tls_arena_fallback_count = TlsArena::fallbackCount();
    *out = info;
}

bool RelayRuntime::maintenanceActive() const {
    return impl_->maintenance || impl_->stop == StopKind::Reset;
}

RelayOpResult RelayRuntime::eraseAllForFactoryReset() {
    Impl& impl = *impl_;
    if (impl.setup_state == RelaySetupState::Pending || impl.setup_state == RelaySetupState::Connecting)
        impl.setup_state = RelaySetupState::Cancelled;
    impl.setup_approval.clear();
    if (!impl.timer.isOpen()) return RelayOpResult::TimerActive;
    if (!impl.running && impl.completed_stop == StopKind::Reset) {
        const RelayOpResult result = impl.stop_result;
        if (result != RelayOpResult::Ok) {
            impl.completed_stop = StopKind::None;
            impl.stop = StopKind::None;
            impl.maintenance = false; // a new physical gesture can retry
        }
        return result;
    }
    impl.authorized = false;
    impl.maintenance = true;
    impl.stop = StopKind::Reset;
    if (impl.core) impl.core->transport.setAllowed(false);
    if (!impl.running) {
        // No worker can write now: reset must not require allocating the
        // relay core or a TLS-task stack, particularly under low memory.
        bool ok = impl.nvs.eraseNamespace(kNsConfig);
        for (const char* ns : kTokenNamespaces) ok = impl.nvs.eraseNamespace(ns) && ok;
        impl.refreshCaches();
        if (impl.core) { impl.core->tokens_ready = false; impl.wipeWires(); }
        if (!ok) {
            impl.setError("relay_cleanup_storage_failed");
            impl.stop = StopKind::None;
            impl.maintenance = false;
        }
        return ok ? RelayOpResult::Ok : RelayOpResult::StorageFailed;
    }
    return RelayOpResult::Accepted;
}

const char* RelayRuntime::setupStateName(RelaySetupState state) {
    switch (state) {
        case RelaySetupState::Idle: return "idle";
        case RelaySetupState::Pending: return "pending";
        case RelaySetupState::Connecting: return "connecting";
        case RelaySetupState::Complete: return "complete";
        case RelaySetupState::Cancelled: return "cancelled";
        case RelaySetupState::Expired: return "expired";
        case RelaySetupState::Failed: return "failed";
    }
    return "failed";
}

const char* RelayRuntime::resultName(RelayOpResult result) {
    switch (result) {
        case RelayOpResult::Ok: return "ok";
        case RelayOpResult::Accepted: return "accepted";
        case RelayOpResult::Busy: return "relay_busy";
        case RelayOpResult::Paused: return "bluetooth_active";
        case RelayOpResult::Conflict: return "relay_job_conflict";
        case RelayOpResult::TimerActive: return "timer_active";
        case RelayOpResult::NotEnrolled: return "relay_not_enrolled";
        case RelayOpResult::NotConnected: return "relay_unavailable";
        case RelayOpResult::NotRunning: return "relay_disabled";
        case RelayOpResult::InvalidRequest: return "invalid_relay_request";
        case RelayOpResult::OriginRequired: return "partner_origin_required";
        case RelayOpResult::StorageFailed: return "storage_failed";
        case RelayOpResult::TableFull: return "link_table_full";
        case RelayOpResult::RateLimited: return "link_rate_limited";
        case RelayOpResult::NotFound: return "link_not_found";
        case RelayOpResult::NotReady: return "link_not_ready";
        case RelayOpResult::Unavailable: return "relay_low_memory";
    }
    return "unknown";
}

const char* RelayRuntime::stateName(SessionState state) {
    switch (state) {
        case SessionState::Disabled: return "disabled";
        case SessionState::NotEnrolled: return "not_enrolled";
        case SessionState::Backoff: return "waiting";
        case SessionState::Connecting: return "connecting";
        case SessionState::AwaitingChallenge:
        case SessionState::HelloSent: return "authenticating";
        case SessionState::Live: return "connected";
        case SessionState::Failed: return "failed";
    }
    return "unknown";
}

}}  // namespace opensmarttimer::relay
#endif
