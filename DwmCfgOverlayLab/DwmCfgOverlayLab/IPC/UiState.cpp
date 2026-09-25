#include "UiState.h"

namespace dwm_overlay {

UiSnapshot UiStateStore::Read() const noexcept {
	for (;;) {
		const LONG published = InterlockedCompareExchange(
			&publishedIndex_, 0, 0);
		InterlockedExchange(&readerIndex_, published);
		MemoryBarrier();
		if (InterlockedCompareExchange(&publishedIndex_, 0, 0) != published) {
			InterlockedExchange(&readerIndex_, -1);
			continue;
		}
		const UiSnapshot result = snapshots_[published];
		MemoryBarrier();
		InterlockedExchange(&readerIndex_, -1);
		return result;
	}
}

void UiStateStore::Publish(const UiSnapshot& snapshot) noexcept {
	AcquireSRWLockExclusive(&writerLock_);
	const LONG published = InterlockedCompareExchange(&publishedIndex_, 0, 0);
	const LONG reader = InterlockedCompareExchange(&readerIndex_, 0, 0);
	LONG target = 0;
	for (; target < 3; ++target) {
		if (target != published && target != reader)
			break;
	}
	if (target == 3)
		target = (published + 1) % 3;
	snapshots_[target] = snapshot;
	MemoryBarrier();
	InterlockedExchange(&publishedIndex_, target);
	ReleaseSRWLockExclusive(&writerLock_);
}

} // namespace dwm_overlay
