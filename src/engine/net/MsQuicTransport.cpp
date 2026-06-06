// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 RoaringBytes
// ── MsQuicTransport implementation ─────────────────────────────────
//
// C API wrapping for sv::net::Transport / Listener / Connection. The
// MsQuic C API is included only by this translation unit; the public
// header stays clean of <msquic.h>.
//
// Implementation notes:
//   1. Process-wide MsQuic API table is reference-counted. Multiple
//      Transport instances in the same process share one MsQuicOpen2
//      result and close it only when the last Transport releases.
//   2. The self-signed loopback certificate goes through the classic
//      pKeyProvInfo path: a persistent NCrypt key in a process-unique
//      named container, referenced from the cert via CRYPT_KEY_PROV_INFO.
//      Transport::stop deletes the container. Simpler than PFX export
//      and it works on every Windows 10+ build we care about.
//   3. Connection and Listener Impl structs live in a unique_ptr inside
//      the public handle. MsQuic callbacks stash the Impl pointer as
//      the handle's context, so the Impl address must be stable across
//      the lifetime of the MsQuic handle — std::unique_ptr<Impl>
//      owned by the public Connection/Listener object satisfies that.
//   4. When a listener receives QUIC_LISTENER_EVENT_NEW_CONNECTION it
//      creates a fresh Connection::Impl, transfers the new HQUIC into
//      it, applies the server configuration, installs its own
//      connectionCallback with the new Impl as context, and stashes the
//      Impl in an accept queue. Tests pull it out via Listener::acceptOne.
//   5. Thread safety: MsQuic worker threads mutate ConnectionStats
//      under an Impl-local mutex; tests read via Connection::stats(),
//      which takes the same mutex and returns a copy.

#include "MsQuicTransport.h"

#ifdef STRATUMV_MSQUIC_AVAILABLE

// ── Platform gating ────────────────────────────────────────────────
// Windows builds go through Schannel and wincrypt/ncrypt for the
// self-signed loopback cert. Linux builds go through OpenSSL and
// write PEM files to /tmp that MsQuic loads via QUIC_CREDENTIAL_TYPE_
// CERTIFICATE_FILE. The cross-platform switches live directly inside
// the helpers below.

#if defined(_WIN32)
// MsQuic pulls in winsock2.h + ws2tcpip.h + windows.h. Keeping
// WIN32_LEAN_AND_MEAN avoids the winsock1/2 collision some callers trip.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#endif  // _WIN32

// msquic.h defines QUIC_API_VERSION_2 and includes the per-platform
// includes (msquic_winuser.h on Windows, msquic_posix.h on Linux).
#include <msquic.h>

#if defined(_WIN32)
// Extra Windows crypto headers for self-signed certificate generation.
// windows.h is already dragged in by msquic_winuser.h; including it
// explicitly keeps the dependency obvious to readers.
#include <windows.h>
#include <wincrypt.h>
#include <ncrypt.h>
#else  // POSIX / Linux
// POSIX networking + sockaddr_in fields (sin_addr.s_addr). Used by
// the listener bind path to set the loopback IPv4 address.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
// OpenSSL for self-signed cert generation. The Linux MsQuic build
// uses OpenSSL 3 for its TLS backend, so linking `libcrypto.so` is a
// zero-cost dependency — it's already loaded by libmsquic.so anyway.
#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif  // _WIN32

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ── Process-wide MsQuic API table (refcounted) ──────────────────────
std::mutex                    g_apiMutex;
const QUIC_API_TABLE*         g_apiTable    = nullptr;
int                           g_apiRefCount = 0;

const QUIC_API_TABLE* acquireApiLocked() {
    if (g_apiRefCount == 0) {
        const void* tablePtr = nullptr;
        QUIC_STATUS status = MsQuicOpenVersion(QUIC_API_VERSION_2, &tablePtr);
        if (QUIC_FAILED(status) || !tablePtr) {
            return nullptr;
        }
        g_apiTable = static_cast<const QUIC_API_TABLE*>(tablePtr);
    }
    g_apiRefCount++;
    return g_apiTable;
}

void releaseApiLocked() {
    if (g_apiRefCount == 0) return;
    g_apiRefCount--;
    if (g_apiRefCount == 0 && g_apiTable != nullptr) {
        MsQuicClose(g_apiTable);
        g_apiTable = nullptr;
    }
}

const QUIC_API_TABLE* acquireApi() {
    std::lock_guard<std::mutex> lk(g_apiMutex);
    return acquireApiLocked();
}

void releaseApi() {
    std::lock_guard<std::mutex> lk(g_apiMutex);
    releaseApiLocked();
}

// ── Self-signed certificate bundle ──────────────────────────────────
// Windows: references a persistent NCrypt key by container name; on
// destruction the container is deleted so repeated test runs do not
// accumulate leftover key material in the user's CNG profile.
// Linux: generates an in-memory RSA keypair + X509 cert via
// OpenSSL and writes cert.pem + key.pem to a process-unique temp
// directory; the directory is rm-rf'd on reset. MsQuic Linux consumes
// the PEM files via QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE.
struct SelfSignedCert {
#if defined(_WIN32)
    PCCERT_CONTEXT context        = nullptr;
    std::wstring   keyContainer;  // unique per Transport instance
#else
    // POSIX: paths to the two PEM files we wrote under /tmp. Both
    // empty when the cert has not been built yet.
    std::string    certPath;
    std::string    keyPath;
#endif

    SelfSignedCert() = default;
    SelfSignedCert(const SelfSignedCert&) = delete;
    SelfSignedCert& operator=(const SelfSignedCert&) = delete;
    SelfSignedCert(SelfSignedCert&& other) noexcept { *this = std::move(other); }
    SelfSignedCert& operator=(SelfSignedCert&& other) noexcept {
        if (this != &other) {
            reset();
#if defined(_WIN32)
            context      = other.context;
            keyContainer = std::move(other.keyContainer);
            other.context = nullptr;
#else
            certPath = std::move(other.certPath);
            keyPath  = std::move(other.keyPath);
#endif
        }
        return *this;
    }

    ~SelfSignedCert() { reset(); }

    void reset() {
#if defined(_WIN32)
        if (context) {
            CertFreeCertificateContext(context);
            context = nullptr;
        }
        if (!keyContainer.empty()) {
            NCRYPT_PROV_HANDLE prov = 0;
            if (NCryptOpenStorageProvider(&prov, MS_KEY_STORAGE_PROVIDER, 0) == ERROR_SUCCESS) {
                NCRYPT_KEY_HANDLE key = 0;
                if (NCryptOpenKey(prov, &key, keyContainer.c_str(), 0, NCRYPT_SILENT_FLAG) == ERROR_SUCCESS) {
                    NCryptDeleteKey(key, NCRYPT_SILENT_FLAG);
                    // NCryptDeleteKey frees the handle on success.
                }
                NCryptFreeObject(prov);
            }
            keyContainer.clear();
        }
#else
        // Delete the two PEM files we wrote under /tmp. std::filesystem
        // is safe for missing files (returns false / no-throw overload).
        std::error_code ec;
        if (!certPath.empty()) {
            std::filesystem::remove(certPath, ec);
            certPath.clear();
        }
        if (!keyPath.empty()) {
            std::filesystem::remove(keyPath, ec);
            keyPath.clear();
        }
#endif
    }
};

#if defined(_WIN32)
bool makeSelfSignedLoopbackCert(SelfSignedCert& out) {
    out.reset();

    // Process-unique key container name — avoids colliding with leftover
    // state from earlier test runs (NCRYPT_OVERWRITE_KEY_FLAG is a
    // second belt-and-suspenders safety net).
    wchar_t containerName[128];
    swprintf(
        containerName,
        sizeof(containerName) / sizeof(containerName[0]),
        L"StratumV-NET1a-%u-%llu",
        static_cast<unsigned>(GetCurrentProcessId()),
        static_cast<unsigned long long>(GetTickCount64()));
    out.keyContainer = containerName;

    NCRYPT_PROV_HANDLE provHandle = 0;
    NCRYPT_KEY_HANDLE  keyHandle  = 0;

    SECURITY_STATUS ss = NCryptOpenStorageProvider(
        &provHandle, MS_KEY_STORAGE_PROVIDER, 0);
    if (ss != ERROR_SUCCESS) {
        out.keyContainer.clear();
        return false;
    }

    ss = NCryptCreatePersistedKey(
        provHandle,
        &keyHandle,
        NCRYPT_RSA_ALGORITHM,
        containerName,
        0,
        NCRYPT_OVERWRITE_KEY_FLAG);
    if (ss != ERROR_SUCCESS) {
        NCryptFreeObject(provHandle);
        out.keyContainer.clear();
        return false;
    }

    DWORD keyLen = 2048;
    ss = NCryptSetProperty(
        keyHandle,
        NCRYPT_LENGTH_PROPERTY,
        reinterpret_cast<PBYTE>(&keyLen),
        sizeof(keyLen),
        NCRYPT_SILENT_FLAG);
    if (ss != ERROR_SUCCESS) {
        NCryptFreeObject(keyHandle);
        NCryptFreeObject(provHandle);
        out.reset();
        return false;
    }

    ss = NCryptFinalizeKey(keyHandle, NCRYPT_SILENT_FLAG);
    if (ss != ERROR_SUCCESS) {
        NCryptFreeObject(keyHandle);
        NCryptFreeObject(provHandle);
        out.reset();
        return false;
    }

    // NCrypt handles are only needed for the key container existence
    // guarantee — the cert references the key by container name, so we
    // can release the handles after Finalize returns.
    NCryptFreeObject(keyHandle);
    NCryptFreeObject(provHandle);

    // Subject name: "CN=StratumVLoopbackTest"
    static const wchar_t* kSubjectName = L"CN=StratumVLoopbackTest";
    DWORD nameEncSize = 0;
    if (!CertStrToNameW(
            X509_ASN_ENCODING,
            kSubjectName,
            CERT_X500_NAME_STR,
            nullptr,
            nullptr,
            &nameEncSize,
            nullptr)) {
        out.reset();
        return false;
    }
    std::vector<BYTE> nameBlob(nameEncSize);
    if (!CertStrToNameW(
            X509_ASN_ENCODING,
            kSubjectName,
            CERT_X500_NAME_STR,
            nullptr,
            nameBlob.data(),
            &nameEncSize,
            nullptr)) {
        out.reset();
        return false;
    }
    CERT_NAME_BLOB subject{};
    subject.cbData = nameEncSize;
    subject.pbData = nameBlob.data();

    // CRYPT_KEY_PROV_INFO points the cert at our persistent CNG key.
    CRYPT_KEY_PROV_INFO keyProvInfo{};
    keyProvInfo.pwszContainerName = const_cast<LPWSTR>(containerName);
    keyProvInfo.pwszProvName      = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
    keyProvInfo.dwProvType        = 0;   // 0 == CNG
    keyProvInfo.dwFlags           = 0;
    keyProvInfo.cProvParam        = 0;
    keyProvInfo.rgProvParam       = nullptr;
    keyProvInfo.dwKeySpec         = 0;

    CRYPT_ALGORITHM_IDENTIFIER sigAlg{};
    sigAlg.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

    // Validity window: a generous 24 hours.
    SYSTEMTIME notBefore{};
    SYSTEMTIME notAfter{};
    GetSystemTime(&notBefore);
    FILETIME notAfterFt{};
    SystemTimeToFileTime(&notBefore, &notAfterFt);
    ULARGE_INTEGER notAfterLi{};
    notAfterLi.LowPart  = notAfterFt.dwLowDateTime;
    notAfterLi.HighPart = notAfterFt.dwHighDateTime;
    notAfterLi.QuadPart += 864000000000ULL;  // 24h in 100ns ticks
    notAfterFt.dwLowDateTime  = notAfterLi.LowPart;
    notAfterFt.dwHighDateTime = notAfterLi.HighPart;
    FileTimeToSystemTime(&notAfterFt, &notAfter);

    out.context = CertCreateSelfSignCertificate(
        0,             // hCryptProvOrNCryptKey: 0 => use pKeyProvInfo
        &subject,
        0,
        &keyProvInfo,
        &sigAlg,
        &notBefore,
        &notAfter,
        nullptr);

    if (!out.context) {
        out.reset();
        return false;
    }
    return true;
}
#else  // POSIX: OpenSSL self-signed cert generator ─────────────────

// RAII holder for temp paths so that early-exit failures don't leak
// half-written files in /tmp. If the full flow succeeds we hand the
// paths over to the SelfSignedCert and clear the holder.
struct TempPemPaths {
    std::string certPath;
    std::string keyPath;

    ~TempPemPaths() {
        std::error_code ec;
        if (!certPath.empty()) std::filesystem::remove(certPath, ec);
        if (!keyPath.empty())  std::filesystem::remove(keyPath,  ec);
    }
};

bool makeSelfSignedLoopbackCert(SelfSignedCert& out) {
    out.reset();

    // Process-unique temp file names. Using rand() instead of getpid()
    // alone because multiple Transport instances in the same process
    // must not collide on cert paths. std::random_device is overkill
    // but it costs nothing on the slow path.
    std::random_device rd;
    std::mt19937_64    rng(rd());
    const uint64_t     rnd = rng();

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "/tmp/stratumv-net1c-%d-%llx-cert.pem",
                  static_cast<int>(::getpid()),
                  static_cast<unsigned long long>(rnd));
    TempPemPaths paths;
    paths.certPath.assign(buf);
    std::snprintf(buf, sizeof(buf),
                  "/tmp/stratumv-net1c-%d-%llx-key.pem",
                  static_cast<int>(::getpid()),
                  static_cast<unsigned long long>(rnd));
    paths.keyPath.assign(buf);

    // ── Step 1: RSA 2048-bit keypair ──────────────────────────────
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!pctx) return false;
    if (EVP_PKEY_keygen_init(pctx) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    if (EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return false;
    }
    EVP_PKEY_CTX_free(pctx);
    pctx = nullptr;

    // ── Step 2: X509 certificate wrapping the key ─────────────────
    X509* cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(pkey);
        return false;
    }
    X509_set_version(cert, 2);  // v3
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert),  60 * 60 * 24);  // 24h
    X509_set_pubkey(cert, pkey);

    // Subject + issuer identical (self-signed).
    X509_NAME* name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("StratumVLoopbackTest"),
        -1, -1, 0);
    X509_set_issuer_name(cert, name);

    if (X509_sign(cert, pkey, EVP_sha256()) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }

    // ── Step 3: Write PEM files under /tmp ─────────────────────────
    FILE* certFp = std::fopen(paths.certPath.c_str(), "wb");
    if (!certFp) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }
    if (PEM_write_X509(certFp, cert) == 0) {
        std::fclose(certFp);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }
    std::fclose(certFp);

    FILE* keyFp = std::fopen(paths.keyPath.c_str(), "wb");
    if (!keyFp) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }
    if (PEM_write_PrivateKey(keyFp, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 0) {
        std::fclose(keyFp);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return false;
    }
    std::fclose(keyFp);

    X509_free(cert);
    EVP_PKEY_free(pkey);

    // ── Step 4: Commit paths to caller and disarm the RAII cleanup ─
    out.certPath = std::move(paths.certPath);
    out.keyPath  = std::move(paths.keyPath);
    paths.certPath.clear();
    paths.keyPath.clear();
    return true;
}
#endif  // _WIN32

// ── QUIC_ADDR → "a.b.c.d:port" helper ───────────────────────────────
// MsQuic gives us QUIC_ADDR unions; we only care about the printable
// form for diagnostic strings on Connection::stats(). The QuicAddr*
// helper inlines live in msquic_winuser.h.
std::string formatQuicAddr(const QUIC_ADDR* addr) {
    if (!addr) return {};
    char buf[64] = {};
    const uint16_t family = QuicAddrGetFamily(addr);
    if (family == QUIC_ADDRESS_FAMILY_INET) {
        // IPv4. Windows wraps sin_addr in a S_un union; Linux exposes
        // sin_addr.s_addr directly. MsQuic still uses the platform's
        // native sockaddr_in layout on each side.
#if defined(_WIN32)
        const uint32_t ipBe = addr->Ipv4.sin_addr.S_un.S_addr;  // network order
#else
        const uint32_t ipBe = addr->Ipv4.sin_addr.s_addr;       // network order
#endif
        const uint8_t b0 = static_cast<uint8_t>( ipBe        & 0xFF);
        const uint8_t b1 = static_cast<uint8_t>((ipBe >>  8) & 0xFF);
        const uint8_t b2 = static_cast<uint8_t>((ipBe >> 16) & 0xFF);
        const uint8_t b3 = static_cast<uint8_t>((ipBe >> 24) & 0xFF);
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u",
            b0, b1, b2, b3, QuicAddrGetPort(addr));
    } else if (family == QUIC_ADDRESS_FAMILY_INET6) {
        // IPv6 — simplified colon-separated hex form, no RFC 5952 ::
        // compression. Good enough for test diagnostic strings.
        const uint16_t* words = reinterpret_cast<const uint16_t*>(
            &addr->Ipv6.sin6_addr);
        std::snprintf(buf, sizeof(buf),
            "[%x:%x:%x:%x:%x:%x:%x:%x]:%u",
            static_cast<unsigned>(ntohs(words[0])),
            static_cast<unsigned>(ntohs(words[1])),
            static_cast<unsigned>(ntohs(words[2])),
            static_cast<unsigned>(ntohs(words[3])),
            static_cast<unsigned>(ntohs(words[4])),
            static_cast<unsigned>(ntohs(words[5])),
            static_cast<unsigned>(ntohs(words[6])),
            static_cast<unsigned>(ntohs(words[7])),
            QuicAddrGetPort(addr));
    } else {
        std::snprintf(buf, sizeof(buf), "unknown-family-%u", family);
    }
    return std::string(buf);
}

} // namespace (anonymous)

// ─── Begin sv::net namespace ────────────────────────────────────────
namespace sv::net {

// Forward declarations for the MsQuic callbacks we hand to MsQuic.
static QUIC_STATUS QUIC_API connectionCallback(HQUIC connHandle, void* context, QUIC_CONNECTION_EVENT* evt);
static QUIC_STATUS QUIC_API listenerCallback(HQUIC listenerHandle, void* context, QUIC_LISTENER_EVENT* evt);
static QUIC_STATUS QUIC_API receiveStreamCallback(HQUIC strmHandle, void* context, QUIC_STREAM_EVENT* evt);
static QUIC_STATUS QUIC_API sendStreamCallback(HQUIC strmHandle, void* context, QUIC_STREAM_EVENT* evt);

// ── Impl structs ────────────────────────────────────────────────────
// Forward declare the per-stream state so Connection::Impl can hold it;
// the full definition follows below.
struct ReceiveStreamState;

struct Connection::Impl {
    const QUIC_API_TABLE*   table             = nullptr;
    HQUIC                   handle            = nullptr;
    bool                    shutdownRequested = false;

    mutable std::mutex              mu;
    mutable std::condition_variable cv;
    ConnectionStats                 stats{};

    // Datagram handler. Protected by mu; the MsQuic worker
    // thread snapshots it under the lock before invoking to avoid
    // races with setDatagramHandler.
    DatagramHandler                 datagramHandler;

    // Reliable message handler. Protected by mu; snapshot
    // under the lock before invoking so a concurrent setter is race-
    // free. The handler fires once per fully-received peer stream.
    ReliableMessageHandler          reliableMessageHandler;

    // Map of in-flight receive streams. Keyed by HQUIC.
    // MsQuic serializes event delivery for a given stream, so the
    // per-entry state does not need its own lock — the map itself is
    // guarded by `mu` for insert / erase. unique_ptr ownership means
    // the state is destroyed exactly when we erase the entry, which
    // happens on SHUTDOWN_COMPLETE for the stream.
    std::unordered_map<HQUIC, std::unique_ptr<ReceiveStreamState>>
                                    receiveStreams;

    ~Impl() {
        if (handle && table) {
            // ConnectionClose serializes with event callbacks: after it
            // returns no further connectionCallback will fire with this
            // context pointer, so it is safe to tear down the Impl.
            // It also closes every associated stream, which means any
            // surviving receive stream state will have been freed via
            // SHUTDOWN_COMPLETE during the close. The receiveStreams
            // map destructor mops up anything MsQuic did not deliver
            // a terminal event for.
            table->ConnectionClose(handle);
            handle = nullptr;
        }
    }
};

// ── Peer-initiated unidirectional receive stream state ───────────────
// One allocated per peer-opened stream. Lifetime is bounded by the
// stream's lifetime on MsQuic's side (PEER_STREAM_STARTED → RECEIVE* →
// PEER_SEND_SHUTDOWN → SHUTDOWN_COMPLETE). The Connection::Impl keeps
// a unique_ptr map keyed by HQUIC so the state is destroyed when we
// erase the entry on SHUTDOWN_COMPLETE — the raw pointer stored as
// the MsQuic callback context is invalidated in lock-step.
struct ReceiveStreamState {
    Connection::Impl*    connImpl = nullptr;  // borrowed, not owning
    std::vector<uint8_t> buffer;               // accumulates segments
    bool                 finReceived = false;
};

// ── DatagramSend payload holder ──────────────────────────────────────
// MsQuic DatagramSend requires the buffer to stay alive until the
// DATAGRAM_SEND_STATE_CHANGED event fires with a final state. We
// allocate a per-send holder on the heap, stash it as the ClientSendContext,
// and free it on the terminal state event.
struct DatagramSendHolder {
    std::vector<uint8_t> bytes;
    QUIC_BUFFER          buffer{};  // { Length, Buffer } view into bytes
};

// ── Reliable send stream holder ──────────────────────────────────────
// StreamOpen + StreamSend require the bytes to stay alive until the
// stream's SHUTDOWN_COMPLETE event fires. We allocate a holder on the
// heap, stash it as both the stream's callback context AND the
// StreamSend ClientContext, and delete it on SHUTDOWN_COMPLETE. The
// holder keeps a borrowed pointer to the QUIC_API_TABLE so the
// SHUTDOWN_COMPLETE handler can call StreamClose without needing the
// (possibly torn-down) Connection::Impl.
struct ReliableSendHolder {
    const QUIC_API_TABLE* table = nullptr;
    std::vector<uint8_t>  bytes;
    QUIC_BUFFER           buffer{};
};

struct Listener::Impl {
    const QUIC_API_TABLE*           table        = nullptr;
    HQUIC                           handle       = nullptr;
    HQUIC                           serverConfig = nullptr;  // borrowed from Transport
    std::string                     alpn;
    uint16_t                        localPort    = 0;
    bool                            stopped      = false;

    mutable std::mutex              mu;
    mutable std::condition_variable cv;
    std::queue<std::unique_ptr<Connection::Impl>> accepted;

    ~Impl() {
        if (handle && table) {
            table->ListenerStop(handle);
            table->ListenerClose(handle);
            handle = nullptr;
        }
        // Drop any connections that the test never pulled out via
        // acceptOne — their Impl destructor handles their HQUIC close.
    }
};

struct Transport::Impl {
    Config                  cfg;
    std::string             appNameStorage;  // stable C-string for RegistrationConfig
    std::string             alpnStr;

    const QUIC_API_TABLE*   table         = nullptr;
    bool                    apiAcquired   = false;
    HQUIC                   registration  = nullptr;
    HQUIC                   clientConfig  = nullptr;
    HQUIC                   serverConfig  = nullptr;
    SelfSignedCert          selfCert;
    bool                    started       = false;
};

// ── TransportStatus strings ─────────────────────────────────────────
const char* transportStatusToString(TransportStatus s) {
    switch (s) {
        case TransportStatus::Ok:                  return "Ok";
        case TransportStatus::InvalidArg:          return "InvalidArg";
        case TransportStatus::NotStarted:          return "NotStarted";
        case TransportStatus::AlreadyStarted:      return "AlreadyStarted";
        case TransportStatus::InitFailed:          return "InitFailed";
        case TransportStatus::RegistrationFailed:  return "RegistrationFailed";
        case TransportStatus::CredentialFailed:    return "CredentialFailed";
        case TransportStatus::ConfigurationFailed: return "ConfigurationFailed";
        case TransportStatus::ListenerFailed:      return "ListenerFailed";
        case TransportStatus::ConnectFailed:       return "ConnectFailed";
        case TransportStatus::MsQuicMissing:       return "MsQuicMissing";
    }
    return "Unknown";
}

// ── Connection ──────────────────────────────────────────────────────
Connection::Connection() : m_impl(std::make_unique<Impl>()) {}
Connection::~Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

bool Connection::valid() const {
    return m_impl && m_impl->handle != nullptr;
}

ConnectionStats Connection::stats() const {
    if (!m_impl) return {};
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->stats;
}

bool Connection::waitForConnected(uint32_t timeoutMs) const {
    if (!m_impl) return false;
    std::unique_lock<std::mutex> lk(m_impl->mu);
    const bool signalled = m_impl->cv.wait_for(
        lk,
        std::chrono::milliseconds(timeoutMs),
        [this]() {
            return m_impl->stats.connected || m_impl->stats.shutdownComplete;
        });
    return signalled && m_impl->stats.connected;
}

bool Connection::waitForShutdownComplete(uint32_t timeoutMs) const {
    if (!m_impl) return false;
    std::unique_lock<std::mutex> lk(m_impl->mu);
    return m_impl->cv.wait_for(
        lk,
        std::chrono::milliseconds(timeoutMs),
        [this]() { return m_impl->stats.shutdownComplete; });
}

void Connection::shutdown(uint64_t errorCode) {
    if (!m_impl || !m_impl->handle || !m_impl->table) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (m_impl->shutdownRequested) return;
        m_impl->shutdownRequested = true;
    }
    m_impl->table->ConnectionShutdown(
        m_impl->handle,
        QUIC_CONNECTION_SHUTDOWN_FLAG_NONE,
        errorCode);
}

// ── Datagram API ───────────────────────────────────────────────────
bool Connection::sendDatagram(const uint8_t* data, size_t size) {
    if (!m_impl || !m_impl->handle || !m_impl->table || !data || size == 0) {
        return false;
    }

    // Gate on "connected" so pre-handshake sends are rejected cleanly.
    // The caller can poll waitForConnected(...) before calling in.
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (!m_impl->stats.connected || m_impl->stats.shutdownStarted) {
            return false;
        }
    }

    // Allocate a heap holder that survives until the terminal send
    // state event. The ClientContext pointer is the holder; the
    // DATAGRAM_SEND_STATE_CHANGED handler frees it on the final state.
    auto holder = std::make_unique<DatagramSendHolder>();
    holder->bytes.assign(data, data + size);
    holder->buffer.Length = static_cast<uint32_t>(holder->bytes.size());
    holder->buffer.Buffer = holder->bytes.data();

    DatagramSendHolder* raw = holder.release();

    QUIC_STATUS status = m_impl->table->DatagramSend(
        m_impl->handle,
        &raw->buffer,
        1,
        QUIC_SEND_FLAG_NONE,
        raw);
    if (QUIC_FAILED(status)) {
        // MsQuic will not fire the terminal event if DatagramSend
        // returned a failure code, so the caller owns the cleanup.
        delete raw;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        m_impl->stats.datagramsSent     += 1;
        m_impl->stats.datagramBytesSent += size;
    }
    return true;
}

void Connection::setDatagramHandler(DatagramHandler handler) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->datagramHandler = std::move(handler);
}

// ── Reliable message API ───────────────────────────────────────────
bool Connection::sendReliableMessage(const uint8_t* data, size_t size) {
    if (!m_impl || !m_impl->handle || !m_impl->table || !data || size == 0) {
        return false;
    }

    // Gate on the connection being in the connected state, same as
    // sendDatagram. StreamOpen pre-handshake is technically supported
    // by MsQuic but layers complexity for zero benefit at this
    // scope (the server polls for connected before issuing the preamble
    // anyway).
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        if (!m_impl->stats.connected || m_impl->stats.shutdownStarted) {
            return false;
        }
    }

    // Holder keeps the send bytes alive until MsQuic releases them via
    // SHUTDOWN_COMPLETE. The holder also borrows the QUIC_API_TABLE so
    // the terminal-state handler can call StreamClose without touching
    // the Connection::Impl.
    auto holder = std::make_unique<ReliableSendHolder>();
    holder->table = m_impl->table;
    holder->bytes.assign(data, data + size);
    holder->buffer.Length = static_cast<uint32_t>(holder->bytes.size());
    holder->buffer.Buffer = holder->bytes.data();

    HQUIC strmHandle = nullptr;
    QUIC_STATUS status = m_impl->table->StreamOpen(
        m_impl->handle,
        QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL,
        sendStreamCallback,
        holder.get(),
        &strmHandle);
    if (QUIC_FAILED(status)) {
        return false;
    }

    status = m_impl->table->StreamStart(
        strmHandle,
        QUIC_STREAM_START_FLAG_IMMEDIATE);
    if (QUIC_FAILED(status)) {
        m_impl->table->StreamClose(strmHandle);
        return false;
    }

    // Send with FIN — the single call is the full message. MsQuic
    // will fire SEND_COMPLETE once the bytes are acknowledged and
    // SHUTDOWN_COMPLETE once the stream is fully drained; the
    // sendStreamCallback frees the holder on the latter.
    status = m_impl->table->StreamSend(
        strmHandle,
        &holder->buffer,
        1,
        QUIC_SEND_FLAG_FIN,
        holder.get());
    if (QUIC_FAILED(status)) {
        // Abort the stream so we still see a terminal SHUTDOWN_COMPLETE
        // event. That event will free the holder via sendStreamCallback,
        // so we must NOT delete it here — release the unique_ptr.
        m_impl->table->StreamShutdown(
            strmHandle,
            QUIC_STREAM_SHUTDOWN_FLAG_ABORT,
            0);
        (void)holder.release();
        return false;
    }

    // Ownership now lives with MsQuic via the SHUTDOWN_COMPLETE path.
    (void)holder.release();

    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        m_impl->stats.reliableMessagesSent += 1;
        m_impl->stats.reliableBytesSent    += size;
    }
    return true;
}

void Connection::setReliableMessageHandler(ReliableMessageHandler handler) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    m_impl->reliableMessageHandler = std::move(handler);
}

// ── Listener ────────────────────────────────────────────────────────
Listener::Listener() : m_impl(std::make_unique<Impl>()) {}
Listener::~Listener() = default;
Listener::Listener(Listener&&) noexcept = default;
Listener& Listener::operator=(Listener&&) noexcept = default;

bool Listener::valid() const {
    return m_impl && m_impl->handle != nullptr;
}

uint16_t Listener::localPort() const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lk(m_impl->mu);
    return m_impl->localPort;
}

Connection Listener::acceptOne(uint32_t timeoutMs) {
    Connection out;
    if (!m_impl) return out;

    std::unique_lock<std::mutex> lk(m_impl->mu);
    const bool got = m_impl->cv.wait_for(
        lk,
        std::chrono::milliseconds(timeoutMs),
        [this]() {
            return !m_impl->accepted.empty() || m_impl->stopped;
        });

    if (!got || m_impl->accepted.empty()) {
        return out;
    }

    out.m_impl = std::move(m_impl->accepted.front());
    m_impl->accepted.pop();
    return out;
}

void Listener::stop() {
    if (!m_impl) return;
    {
        std::lock_guard<std::mutex> lk(m_impl->mu);
        m_impl->stopped = true;
        if (m_impl->handle && m_impl->table) {
            m_impl->table->ListenerStop(m_impl->handle);
        }
    }
    m_impl->cv.notify_all();
}

// ── Transport ───────────────────────────────────────────────────────
Transport::Transport() : m_impl(std::make_unique<Impl>()) {}
Transport::~Transport() {
    if (m_impl && m_impl->started) {
        stop();
    }
}

bool Transport::started() const {
    return m_impl && m_impl->started;
}

bool Transport::isMsquicAvailable() {
    return true;
}

std::string Transport::msquicVersionString() {
    // Pinned version string matches CMakeLists.txt's FetchContent tag.
    return "2.5.6";
}

TransportStatus Transport::start(const Config& cfg) {
    if (!m_impl) return TransportStatus::InvalidArg;
    if (m_impl->started) return TransportStatus::AlreadyStarted;

    m_impl->cfg            = cfg;
    m_impl->appNameStorage = cfg.appName.empty() ? std::string("stratumv") : cfg.appName;
    m_impl->alpnStr        = cfg.alpn.empty()    ? std::string("stratumv/1") : cfg.alpn;

    // 1. MsQuic API table (process singleton, refcounted).
    m_impl->table = acquireApi();
    if (!m_impl->table) {
        return TransportStatus::InitFailed;
    }
    m_impl->apiAcquired = true;

    // 2. Registration.
    QUIC_REGISTRATION_CONFIG regConfig{};
    regConfig.AppName          = m_impl->appNameStorage.c_str();
    regConfig.ExecutionProfile = QUIC_EXECUTION_PROFILE_LOW_LATENCY;

    QUIC_STATUS status = m_impl->table->RegistrationOpen(
        &regConfig, &m_impl->registration);
    if (QUIC_FAILED(status)) {
        stop();
        return TransportStatus::RegistrationFailed;
    }

    // 3. Base settings (idle timeout + peer stream count).
    QUIC_SETTINGS settings{};
    if (cfg.idleTimeoutMs > 0) {
        settings.IdleTimeoutMs       = cfg.idleTimeoutMs;
        settings.IsSet.IdleTimeoutMs = TRUE;
    }
    settings.PeerBidiStreamCount        = 64;
    settings.IsSet.PeerBidiStreamCount  = TRUE;
    settings.PeerUnidiStreamCount       = 64;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    // Enable unreliable datagrams (RFC 9221). The peer
    // must set this too (both sides do via their own Transport::Config).
    // MsQuic negotiates the max datagram payload during the TLS
    // handshake and surfaces it via DATAGRAM_STATE_CHANGED.
    settings.DatagramReceiveEnabled       = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;

    // ALPN buffer. MsQuic stores the pointer for the lifetime of the
    // configuration handle, so the backing std::string (alpnStr) must
    // outlive the configuration — Transport::Impl holds it.
    QUIC_BUFFER alpnBuf{};
    alpnBuf.Length = static_cast<uint32_t>(m_impl->alpnStr.size());
    alpnBuf.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(m_impl->alpnStr.data()));

    // 4. Server configuration (only when we have a cert).
    if (cfg.useSelfSignedLoopbackCert) {
        if (!makeSelfSignedLoopbackCert(m_impl->selfCert)) {
            stop();
            return TransportStatus::CredentialFailed;
        }

        QUIC_CREDENTIAL_CONFIG cred{};
        cred.Flags = QUIC_CREDENTIAL_FLAG_NONE;

#if defined(_WIN32)
        // Windows (Schannel): pass the PCCERT_CONTEXT directly via the
        // CertificateContext slot. The cert references a persistent
        // CNG key container for the private key.
        cred.Type                = QUIC_CREDENTIAL_TYPE_CERTIFICATE_CONTEXT;
        cred.CertificateContext  = const_cast<QUIC_CERTIFICATE*>(
            reinterpret_cast<const QUIC_CERTIFICATE*>(m_impl->selfCert.context));
#else
        // Linux (OpenSSL): pass the two PEM file paths we generated
        // under /tmp via the CertificateFile slot. The path storage
        // must outlive the ConfigurationLoadCredential call, which it
        // does because m_impl->selfCert owns the strings for the full
        // Transport lifetime.
        QUIC_CERTIFICATE_FILE certFile{};
        certFile.CertificateFile = m_impl->selfCert.certPath.c_str();
        certFile.PrivateKeyFile  = m_impl->selfCert.keyPath.c_str();
        cred.Type                 = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
        cred.CertificateFile      = &certFile;
#endif

        QUIC_SETTINGS srvSettings        = settings;
        srvSettings.ServerResumptionLevel        = QUIC_SERVER_RESUME_AND_ZERORTT;
        srvSettings.IsSet.ServerResumptionLevel  = TRUE;

        status = m_impl->table->ConfigurationOpen(
            m_impl->registration,
            &alpnBuf, 1,
            &srvSettings, sizeof(srvSettings),
            nullptr,
            &m_impl->serverConfig);
        if (QUIC_FAILED(status)) {
            stop();
            return TransportStatus::ConfigurationFailed;
        }

        status = m_impl->table->ConfigurationLoadCredential(
            m_impl->serverConfig, &cred);
        if (QUIC_FAILED(status)) {
            stop();
            return TransportStatus::CredentialFailed;
        }
    }

    // 5. Client configuration — always built so connect() can work.
    {
        QUIC_CREDENTIAL_CONFIG cred{};
        cred.Type  = QUIC_CREDENTIAL_TYPE_NONE;
        cred.Flags = QUIC_CREDENTIAL_FLAG_CLIENT;
        if (cfg.clientInsecureNoVerify) {
            cred.Flags |= QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
        }

        status = m_impl->table->ConfigurationOpen(
            m_impl->registration,
            &alpnBuf, 1,
            &settings, sizeof(settings),
            nullptr,
            &m_impl->clientConfig);
        if (QUIC_FAILED(status)) {
            stop();
            return TransportStatus::ConfigurationFailed;
        }

        status = m_impl->table->ConfigurationLoadCredential(
            m_impl->clientConfig, &cred);
        if (QUIC_FAILED(status)) {
            stop();
            return TransportStatus::CredentialFailed;
        }
    }

    m_impl->started = true;
    return TransportStatus::Ok;
}

void Transport::stop() {
    if (!m_impl) return;
    m_impl->started = false;

    if (m_impl->table) {
        if (m_impl->serverConfig) {
            m_impl->table->ConfigurationClose(m_impl->serverConfig);
            m_impl->serverConfig = nullptr;
        }
        if (m_impl->clientConfig) {
            m_impl->table->ConfigurationClose(m_impl->clientConfig);
            m_impl->clientConfig = nullptr;
        }
        if (m_impl->registration) {
            m_impl->table->RegistrationClose(m_impl->registration);
            m_impl->registration = nullptr;
        }
    }

    // Self-signed cert cleanup (deletes the CNG key container).
    m_impl->selfCert.reset();

    if (m_impl->apiAcquired) {
        releaseApi();
        m_impl->apiAcquired = false;
        m_impl->table = nullptr;
    }
}

TransportStatus Transport::startListener(uint16_t port, Listener& out) {
    if (!m_impl || !m_impl->started) return TransportStatus::NotStarted;
    if (!m_impl->serverConfig)       return TransportStatus::CredentialFailed;
    if (!out.m_impl)                  return TransportStatus::InvalidArg;

    Listener::Impl& limpl = *out.m_impl;
    limpl.table        = m_impl->table;
    limpl.serverConfig = m_impl->serverConfig;
    limpl.alpn         = m_impl->alpnStr;

    QUIC_STATUS status = m_impl->table->ListenerOpen(
        m_impl->registration,
        listenerCallback,
        &limpl,
        &limpl.handle);
    if (QUIC_FAILED(status)) {
        return TransportStatus::ListenerFailed;
    }

    // Bind to 127.0.0.1:<port>. The sin_addr field path differs
    // between Windows (S_un union) and POSIX (direct s_addr member);
    // htonl is POSIX-portable (winsock exposes it too).
    QUIC_ADDR addr{};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_INET);
    QuicAddrSetPort(&addr, port);
#if defined(_WIN32)
    addr.Ipv4.sin_addr.S_un.S_addr = htonl(0x7F000001);  // 127.0.0.1
#else
    addr.Ipv4.sin_addr.s_addr      = htonl(0x7F000001);  // 127.0.0.1
#endif

    QUIC_BUFFER alpnBuf{};
    alpnBuf.Length = static_cast<uint32_t>(limpl.alpn.size());
    alpnBuf.Buffer = reinterpret_cast<uint8_t*>(const_cast<char*>(limpl.alpn.data()));

    status = m_impl->table->ListenerStart(limpl.handle, &alpnBuf, 1, &addr);
    if (QUIC_FAILED(status)) {
        m_impl->table->ListenerClose(limpl.handle);
        limpl.handle = nullptr;
        return TransportStatus::ListenerFailed;
    }

    // Fetch the actual bound address (ephemeral port resolution).
    QUIC_ADDR boundAddr{};
    uint32_t  boundSize = sizeof(boundAddr);
    if (m_impl->table->GetParam(
            limpl.handle,
            QUIC_PARAM_LISTENER_LOCAL_ADDRESS,
            &boundSize,
            &boundAddr) == QUIC_STATUS_SUCCESS) {
        std::lock_guard<std::mutex> lk(limpl.mu);
        limpl.localPort = QuicAddrGetPort(&boundAddr);
    }

    return TransportStatus::Ok;
}

TransportStatus Transport::connect(const std::string& host, uint16_t port, Connection& out) {
    if (!m_impl || !m_impl->started) return TransportStatus::NotStarted;
    if (!m_impl->clientConfig)       return TransportStatus::ConfigurationFailed;
    if (!out.m_impl)                  return TransportStatus::InvalidArg;

    Connection::Impl& cimpl = *out.m_impl;
    cimpl.table = m_impl->table;

    QUIC_STATUS status = m_impl->table->ConnectionOpen(
        m_impl->registration,
        connectionCallback,
        &cimpl,
        &cimpl.handle);
    if (QUIC_FAILED(status)) {
        return TransportStatus::ConnectFailed;
    }

    status = m_impl->table->ConnectionStart(
        cimpl.handle,
        m_impl->clientConfig,
        QUIC_ADDRESS_FAMILY_UNSPEC,
        host.c_str(),
        port);
    if (QUIC_FAILED(status)) {
        m_impl->table->ConnectionClose(cimpl.handle);
        cimpl.handle = nullptr;
        return TransportStatus::ConnectFailed;
    }

    return TransportStatus::Ok;
}

// ── MsQuic callbacks ────────────────────────────────────────────────
static QUIC_STATUS QUIC_API
connectionCallback(HQUIC connHandle, void* context, QUIC_CONNECTION_EVENT* evt) {
    auto* impl = static_cast<Connection::Impl*>(context);
    if (!impl || !evt) return QUIC_STATUS_SUCCESS;

    switch (evt->Type) {
        case QUIC_CONNECTION_EVENT_CONNECTED: {
            std::lock_guard<std::mutex> lk(impl->mu);
            impl->stats.connected = true;

            QUIC_ADDR peerAddr{};
            uint32_t  addrSize = sizeof(peerAddr);
            if (impl->table && impl->table->GetParam(
                    connHandle,
                    QUIC_PARAM_CONN_REMOTE_ADDRESS,
                    &addrSize,
                    &peerAddr) == QUIC_STATUS_SUCCESS) {
                impl->stats.peerAddress = formatQuicAddr(&peerAddr);
            }

            // NegotiatedAlpn is valid during the CONNECTED event.
            if (evt->CONNECTED.NegotiatedAlpnLength > 0 &&
                evt->CONNECTED.NegotiatedAlpn != nullptr) {
                impl->stats.negotiatedAlpn.assign(
                    reinterpret_cast<const char*>(evt->CONNECTED.NegotiatedAlpn),
                    static_cast<size_t>(evt->CONNECTED.NegotiatedAlpnLength));
            }

            impl->cv.notify_all();
            break;
        }

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT: {
            std::lock_guard<std::mutex> lk(impl->mu);
            impl->stats.shutdownStarted = true;
            impl->stats.errorCode       =
                static_cast<uint64_t>(evt->SHUTDOWN_INITIATED_BY_TRANSPORT.Status);
            impl->cv.notify_all();
            break;
        }

        case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER: {
            std::lock_guard<std::mutex> lk(impl->mu);
            impl->stats.shutdownStarted = true;
            impl->stats.errorCode       = evt->SHUTDOWN_INITIATED_BY_PEER.ErrorCode;
            impl->cv.notify_all();
            break;
        }

        case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
            std::lock_guard<std::mutex> lk(impl->mu);
            impl->stats.shutdownComplete = true;
            impl->cv.notify_all();
            break;
        }

        // Datagram negotiation state. Fires whenever the
        // peer's receive capability changes (once at handshake +
        // again as MTU probing widens the max send length). We don't
        // currently surface this — the sendDatagram() call gates on
        // stats.connected, and MsQuic will itself reject sends if
        // the peer hasn't enabled reception.
        case QUIC_CONNECTION_EVENT_DATAGRAM_STATE_CHANGED: {
            // Intentionally no-op for now.
            // A future revision can add a stats-surfacing path for this.
            break;
        }

        // Inbound unreliable datagram. Snapshot the handler
        // under the lock so a setDatagramHandler() race cannot null it
        // out while we're holding a raw reference. Then invoke OUTSIDE
        // the lock so a slow handler does not deadlock reconfiguration.
        case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
            const QUIC_BUFFER* buf = evt->DATAGRAM_RECEIVED.Buffer;
            const size_t bytes = buf ? buf->Length : 0;
            DatagramHandler handlerCopy;
            {
                std::lock_guard<std::mutex> lk(impl->mu);
                impl->stats.datagramsReceived      += 1;
                impl->stats.datagramBytesReceived  += bytes;
                handlerCopy = impl->datagramHandler;
            }
            if (handlerCopy && buf && buf->Buffer && buf->Length > 0) {
                handlerCopy(buf->Buffer, buf->Length);
            }
            break;
        }

        // Terminal state on a previously-issued DatagramSend.
        // The ClientContext is the DatagramSendHolder we allocated in
        // Connection::sendDatagram; free it now that MsQuic is done
        // with the bytes.
        case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
            const QUIC_DATAGRAM_SEND_STATE state =
                evt->DATAGRAM_SEND_STATE_CHANGED.State;
            if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(state)) {
                auto* holder = static_cast<DatagramSendHolder*>(
                    evt->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
                delete holder;
            }
            break;
        }

        // Peer opened a stream to us. We only accept
        // unidirectional streams for now — reliable messages flow
        // one-way (server → client in the schema-handshake path).
        // Allocate a per-stream state blob, wire the stream's
        // callback handler, and stash the state in the Connection's
        // receive-stream map so we can look it up on subsequent
        // events. Bidirectional peer streams are rejected via
        // QUIC_STATUS_NOT_SUPPORTED.
        case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
            const QUIC_STREAM_OPEN_FLAGS flags =
                evt->PEER_STREAM_STARTED.Flags;
            if ((flags & QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL) == 0) {
                return QUIC_STATUS_NOT_SUPPORTED;
            }
            HQUIC strmHandle = evt->PEER_STREAM_STARTED.Stream;

            auto state = std::make_unique<ReceiveStreamState>();
            state->connImpl = impl;

            // Route the stream's events to our callback. MsQuic takes
            // a raw pointer; the unique_ptr owns the memory via the
            // map we insert into immediately after.
            if (impl->table) {
                impl->table->SetCallbackHandler(
                    strmHandle,
                    reinterpret_cast<void*>(&receiveStreamCallback),
                    state.get());
            }

            {
                std::lock_guard<std::mutex> lk(impl->mu);
                impl->receiveStreams[strmHandle] = std::move(state);
            }
            return QUIC_STATUS_SUCCESS;
        }

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ── Receive stream callback ────────────────────────────────────────
// Fires on the MsQuic worker thread for every event raised on a
// peer-initiated unidirectional stream. Context is the
// ReceiveStreamState* we set up in QUIC_CONNECTION_EVENT_PEER_STREAM_
// STARTED. The state's lifetime is tied to its entry in
// Connection::Impl::receiveStreams, which we erase on
// QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE.
static QUIC_STATUS QUIC_API
receiveStreamCallback(HQUIC strmHandle, void* context, QUIC_STREAM_EVENT* evt) {
    auto* state = static_cast<ReceiveStreamState*>(context);
    if (!state || !evt) return QUIC_STATUS_SUCCESS;

    switch (evt->Type) {
        case QUIC_STREAM_EVENT_RECEIVE: {
            // Append every buffer segment in order. MsQuic may deliver
            // multiple contiguous buffers in one event; the vector grows
            // to hold them. No lock needed — MsQuic serializes events
            // for a given stream, so we are the sole writer.
            for (uint32_t i = 0; i < evt->RECEIVE.BufferCount; ++i) {
                const QUIC_BUFFER& b = evt->RECEIVE.Buffers[i];
                if (b.Buffer && b.Length > 0) {
                    state->buffer.insert(
                        state->buffer.end(),
                        b.Buffer,
                        b.Buffer + b.Length);
                }
            }
            break;
        }

        case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN: {
            // FIN received — the full message is now in state->buffer.
            // Dispatch to the connection's reliable-message handler.
            state->finReceived = true;

            Connection::Impl* impl = state->connImpl;
            if (!impl) break;

            ReliableMessageHandler handlerCopy;
            std::vector<uint8_t>   delivered;
            {
                std::lock_guard<std::mutex> lk(impl->mu);
                impl->stats.reliableMessagesReceived += 1;
                impl->stats.reliableBytesReceived    += state->buffer.size();
                handlerCopy = impl->reliableMessageHandler;
            }
            // Move the payload out before we drop the lock so subsequent
            // events see an empty buffer on this state.
            delivered = std::move(state->buffer);

            // Invoke the handler OUTSIDE the Connection lock so a slow
            // handler does not stall setReliableMessageHandler or
            // stats() callers. Matches the datagram path.
            if (handlerCopy && !delivered.empty()) {
                handlerCopy(delivered.data(), delivered.size());
            }
            break;
        }

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
            // Stream fully done. Close the handle and erase our state
            // entry. After erase, `state` is dangling — bail out.
            Connection::Impl* impl = state->connImpl;
            if (impl && impl->table) {
                impl->table->StreamClose(strmHandle);
            }
            if (impl) {
                std::lock_guard<std::mutex> lk(impl->mu);
                impl->receiveStreams.erase(strmHandle);
                // state has been destroyed — do NOT touch below.
            }
            return QUIC_STATUS_SUCCESS;
        }

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

// ── Send stream callback ───────────────────────────────────────────
// Fires on the MsQuic worker thread for events on a locally-opened
// unidirectional send stream. Context is the ReliableSendHolder we
// allocated in Connection::sendReliableMessage. Lifetime is bounded
// by SHUTDOWN_COMPLETE — we free the holder there, after which the
// caller has no remaining reference.
static QUIC_STATUS QUIC_API
sendStreamCallback(HQUIC strmHandle, void* context, QUIC_STREAM_EVENT* evt) {
    auto* holder = static_cast<ReliableSendHolder*>(context);
    if (!evt) return QUIC_STATUS_SUCCESS;

    switch (evt->Type) {
        case QUIC_STREAM_EVENT_SEND_COMPLETE:
            // Bytes are acknowledged. We still need SHUTDOWN_COMPLETE
            // before we can free the holder + close the stream handle.
            break;

        case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE: {
            // Cache the borrowed table pointer so we can call
            // StreamClose after deleting the holder.
            const QUIC_API_TABLE* table =
                holder ? holder->table : nullptr;
            delete holder;
            if (table) {
                table->StreamClose(strmHandle);
            }
            return QUIC_STATUS_SUCCESS;
        }

        default:
            break;
    }

    return QUIC_STATUS_SUCCESS;
}

static QUIC_STATUS QUIC_API
listenerCallback(HQUIC /*listenerHandle*/, void* context, QUIC_LISTENER_EVENT* evt) {
    auto* impl = static_cast<Listener::Impl*>(context);
    if (!impl || !evt) return QUIC_STATUS_NOT_SUPPORTED;

    switch (evt->Type) {
        case QUIC_LISTENER_EVENT_NEW_CONNECTION: {
            HQUIC newConnHandle = evt->NEW_CONNECTION.Connection;

            auto newImpl = std::make_unique<Connection::Impl>();
            newImpl->table  = impl->table;
            newImpl->handle = newConnHandle;

            // Route all future events on this HQUIC to our callback
            // with the freshly-allocated Impl as context.
            impl->table->SetCallbackHandler(
                newConnHandle,
                reinterpret_cast<void*>(&connectionCallback),
                newImpl.get());

            // Apply the server configuration so the TLS/QUIC handshake
            // can complete. Failure here rejects the connection.
            QUIC_STATUS status = impl->table->ConnectionSetConfiguration(
                newConnHandle,
                impl->serverConfig);
            if (QUIC_FAILED(status)) {
                newImpl->handle = nullptr;  // MsQuic will clean up the HQUIC
                return status;
            }

            // Stash the new impl in the accept queue and wake any
            // acceptOne waiter.
            {
                std::lock_guard<std::mutex> lk(impl->mu);
                impl->accepted.push(std::move(newImpl));
            }
            impl->cv.notify_all();

            return QUIC_STATUS_SUCCESS;
        }
        default:
            return QUIC_STATUS_NOT_SUPPORTED;
    }
}

} // namespace sv::net

#else // ── STRATUMV_MSQUIC_AVAILABLE fallback (stub build) ─────────

namespace sv::net {

const char* transportStatusToString(TransportStatus s) {
    switch (s) {
        case TransportStatus::Ok:                  return "Ok";
        case TransportStatus::MsQuicMissing:       return "MsQuicMissing";
        default:                                    return "MsQuicMissing";
    }
}

struct Connection::Impl {};
struct Listener::Impl {};
struct Transport::Impl {};

Connection::Connection() = default;
Connection::~Connection() = default;
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;
bool Connection::valid() const { return false; }
ConnectionStats Connection::stats() const { return {}; }
bool Connection::waitForConnected(uint32_t) const { return false; }
bool Connection::waitForShutdownComplete(uint32_t) const { return false; }
void Connection::shutdown(uint64_t) {}
bool Connection::sendDatagram(const uint8_t*, size_t) { return false; }
void Connection::setDatagramHandler(DatagramHandler) {}
bool Connection::sendReliableMessage(const uint8_t*, size_t) { return false; }
void Connection::setReliableMessageHandler(ReliableMessageHandler) {}

Listener::Listener() = default;
Listener::~Listener() = default;
Listener::Listener(Listener&&) noexcept = default;
Listener& Listener::operator=(Listener&&) noexcept = default;
bool Listener::valid() const { return false; }
uint16_t Listener::localPort() const { return 0; }
Connection Listener::acceptOne(uint32_t) { return {}; }
void Listener::stop() {}

Transport::Transport() = default;
Transport::~Transport() = default;
bool Transport::started() const { return false; }
bool Transport::isMsquicAvailable() { return false; }
std::string Transport::msquicVersionString() { return {}; }
TransportStatus Transport::start(const Config&) { return TransportStatus::MsQuicMissing; }
void Transport::stop() {}
TransportStatus Transport::startListener(uint16_t, Listener&) { return TransportStatus::MsQuicMissing; }
TransportStatus Transport::connect(const std::string&, uint16_t, Connection&) { return TransportStatus::MsQuicMissing; }

} // namespace sv::net

#endif // STRATUMV_MSQUIC_AVAILABLE
