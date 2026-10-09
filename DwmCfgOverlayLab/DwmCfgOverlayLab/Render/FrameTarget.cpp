#include "FrameTarget.h"
namespace dwm_overlay {
UINT64 ResourceIdentity(ID3D11Texture2D* texture) noexcept {
    // Private metadata, not a retained GetBuffer reference. A recreated buffer
    // gets a fresh identity even when the COM address is reused.
    static const GUID key = { 0x790eed42, 0xa05e, 0x4fb6, {0xbd,0x4a,0xaf,0x42,0xde,0xbc,0x11,0x53} };
    static volatile LONG64 next = 0;
    static SRWLOCK lock = SRWLOCK_INIT;
    static const UINT64 epoch = []() noexcept {
        LARGE_INTEGER tick = {}; QueryPerformanceCounter(&tick);
        return static_cast<UINT64>(tick.QuadPart);
    }();
    struct Identity { UINT64 epoch, token; } identity = {};
    UINT size = sizeof(identity);
    if (SUCCEEDED(texture->GetPrivateData(key, &size, &identity)) &&
        size == sizeof(identity) && identity.epoch == epoch && identity.token)
        return identity.token;
    // Two presentation callbacks can discover one texture concurrently.
    AcquireSRWLockExclusive(&lock);
    size = sizeof(identity);
    if (FAILED(texture->GetPrivateData(key, &size, &identity)) || size != sizeof(identity) ||
        identity.epoch != epoch || !identity.token) {
        identity = { epoch, static_cast<UINT64>(InterlockedIncrement64(&next)) };
        if (FAILED(texture->SetPrivateData(key, sizeof(identity), &identity))) identity.token = 0;
    }
    ReleaseSRWLockExclusive(&lock);
    return identity.token;
}
} // namespace dwm_overlay
