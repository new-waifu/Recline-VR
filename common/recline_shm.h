// Shared-memory protocol /recline_v1 between driver_recline and its clients
// (recline_overlay, reclinectl, the XRService shim); see docs/DESIGN.md,
// "Shared-memory protocol". Fields are plain integers and doubles (the layout is
// pinned below) accessed through std::atomic_ref, which compiles to plain
// lock-free loads and stores on aarch64/x86-64.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>


namespace recline {

constexpr uint32_t kMagic = 0x52434C4Eu;  // 'RCLN'
constexpr uint32_t kVersion = 1;
constexpr const char* kShmName = "/recline_v1";
constexpr size_t kShmSize = 4096;

enum Cmd : uint32_t { CMD_NONE = 0, CMD_RECENTER = 1, CMD_RESET = 2, CMD_SET = 3 };
enum CmdResult : uint32_t { RES_OK = 0, RES_NO_HMD_POSE = 1, RES_INVALID_ARGS = 2 };
enum StateFlags : uint32_t {
  FLAG_HOOK_INSTALLED = 1u << 0,
  FLAG_TRANSFORM_ACTIVE = 1u << 1,
  FLAG_CONFLICT_DOWNSTREAM = 1u << 2,
  FLAG_WRAPPED = 1u << 3,
  FLAG_HOOK_REFUSED = 1u << 4,
};

struct alignas(64) CmdBlock {  // writers: overlay, reclinectl (multi-writer: lock via seq CAS)
  uint32_t seq;
  uint32_t cmd_id;  // incremented by the writer under the lock
  uint32_t cmd;     // Cmd
  uint32_t _pad;
  double q[4];  // RECENTER: Traw rotation (w,x,y,z); SET: R rotation
  double t[3];  // RECENTER: Traw position (m);       SET: R translation
};

struct alignas(64) StateBlock {  // single writer: driver command thread
  uint32_t seq;
  uint32_t cmd_ack;     // last cmd_id applied (or rejected)
  uint32_t cmd_result;  // CmdResult
  uint32_t flags;       // StateFlags
  uint32_t hmd_index;
  uint32_t _pad;
  double R_q[4], R_t[3];  // current transform
};

struct alignas(64) HmdBlock {  // writers: hook threads (try-lock via seq CAS, never spin)
  uint32_t seq;
  uint32_t _pad;
  double q[4], t[3];    // latest driver-world head pose H
  double time;          // CLOCK_MONOTONIC seconds at publish
  uint64_t pose_count;  // hook calls for the HMD (liveness)
};

struct ReclineShm {
  uint32_t magic;  // published last during init
  uint32_t version;
  CmdBlock cmd;
  StateBlock state;
  HmdBlock hmd;
};

static_assert(sizeof(ReclineShm) <= kShmSize, "shm layout too large");
// Two separately compiled processes share this layout: pin it.
static_assert(offsetof(ReclineShm, cmd) == 64, "layout");
static_assert(offsetof(ReclineShm, state) == 192, "layout");
static_assert(offsetof(ReclineShm, hmd) == 320, "layout");
static_assert(sizeof(CmdBlock) == 128 && sizeof(StateBlock) == 128 && sizeof(HmdBlock) == 128,
              "layout");
static_assert(offsetof(CmdBlock, q) == 16 && offsetof(CmdBlock, t) == 48, "layout");
static_assert(offsetof(StateBlock, R_q) == 24 && offsetof(StateBlock, R_t) == 56, "layout");
static_assert(offsetof(HmdBlock, q) == 8 && offsetof(HmdBlock, time) == 64 &&
                  offsetof(HmdBlock, pose_count) == 72,
              "layout");

// Lock-based atomics would not work across processes.
static_assert(std::atomic_ref<uint32_t>::is_always_lock_free, "u32 atomics must be lock-free");
static_assert(std::atomic_ref<uint64_t>::is_always_lock_free, "u64 atomics must be lock-free");
static_assert(std::atomic_ref<double>::is_always_lock_free, "double atomics must be lock-free");

// ---------------------------------------------------------------------------
// Atomic field access

inline void CpuRelax() {
#if defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#endif
}

template <class T>
inline std::atomic_ref<T> Atomic(T& r) {
  static_assert(alignof(T) >= std::atomic_ref<T>::required_alignment, "misaligned field");
  return std::atomic_ref<T>(r);
}
// Loads from const fields (the shim maps the segment read-only): an atomic load
// never writes, and std::atomic_ref<const T> only arrives in C++26.
template <class T>
inline T Ld(const T& r, std::memory_order o = std::memory_order_relaxed) {
  return Atomic(const_cast<T&>(r)).load(o);
}
template <class T>
inline void St(T& r, T v, std::memory_order o = std::memory_order_relaxed) {
  Atomic(r).store(v, o);
}

// ---------------------------------------------------------------------------
// Seqlock protocol

// Reader. `copy` copies every payload field into locals with Ld(). Returns
// false if no consistent snapshot was obtained within maxAttempts.
template <class CopyFn>
inline bool SeqRead(const uint32_t& seq, CopyFn&& copy, int maxAttempts) {
  for (int i = 0; i < maxAttempts; ++i) {
    const uint32_t s1 = Ld(seq, std::memory_order_acquire);  // (1)
    if (s1 & 1u) {  // writer active: loop before reading the payload
      CpuRelax();
      continue;
    }
    copy();                                               // (2)
    std::atomic_thread_fence(std::memory_order_acquire);  // (3)
    const uint32_t s2 = Ld(seq);                          // (4)
    if (s2 == s1) return true;
    CpuRelax();
  }
  return false;
}

// Single-writer begin: returns the even value to pass to SeqWriteEnd.
inline uint32_t SeqWriteBeginSingle(uint32_t& seq) {
  const uint32_t s = Ld(seq);  // only we write it: even
  St(seq, s + 1);
  std::atomic_thread_fence(std::memory_order_release);  // odd store before payload stores
  return s;
}

// Multi-writer try-lock, one pass: fails immediately if another writer holds
// the block. Used by the hot path, which must never spin.
inline bool SeqTryLock(uint32_t& seq, uint32_t* out) {
  uint32_t s = Ld(seq);
  for (;;) {
    if (s & 1u) return false;
    // On failure `s` is reloaded; loop re-checks parity (spurious failures of
    // the weak CAS are retried, real contention shows up as odd).
    if (Atomic(seq).compare_exchange_weak(s, s + 1, std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
      std::atomic_thread_fence(std::memory_order_release);
      *out = s;
      return true;
    }
  }
}

inline void SeqWriteEnd(uint32_t& seq, uint32_t s) {
  St(seq, s + 2, std::memory_order_release);  // payload stores happen-before this
}

// ---------------------------------------------------------------------------
// Helpers

// The protocol's clock (HmdBlock::time is compared across processes), named
// explicitly rather than relying on which clock std::chrono::steady_clock uses.
inline double MonotonicSeconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

struct CmdSnapshot {
  uint32_t cmd_id = 0, cmd = 0;
  double q[4] = {1, 0, 0, 0}, t[3] = {0, 0, 0};
};
struct StateSnapshot {
  uint32_t cmd_ack = 0, cmd_result = 0, flags = 0, hmd_index = 0;
  double R_q[4] = {1, 0, 0, 0}, R_t[3] = {0, 0, 0};
};
struct HmdSnapshot {
  double q[4] = {1, 0, 0, 0}, t[3] = {0, 0, 0};
  double time = 0;
  uint64_t pose_count = 0;
};

// Seqlock readers; return false if no consistent snapshot was obtained within
// `attempts` (a writer is active or died mid-write).
bool ReadCmd(const CmdBlock& b, CmdSnapshot* o, int attempts = 1000);
bool ReadState(const StateBlock& b, StateSnapshot* o, int attempts = 100000);
bool ReadHmd(const HmdBlock& b, HmdSnapshot* o, int attempts = 100000);

// Single writer (driver command thread).
void WriteState(StateBlock& b, const StateSnapshot& v);

// Multi-writer command submit (overlay, reclinectl). Spins/yields while
// another writer holds the block. The driver force-releases a block left odd
// for > 1 s by a crashed writer, so waiting up to timeoutSec (> 1 s) recovers.
// Returns the cmd_id written, or 0 on timeout.
uint32_t SubmitCmd(CmdBlock& b, uint32_t cmd, const double q[4], const double t[3],
                   double timeoutSec = 2.0);

// Open / create the segment: the creator publishes `magic` last with release
// ordering; openers wait briefly for it and refuse a version mismatch.
//
// takeOverUninitialized: the driver passes true. If an existing segment never
// gets its magic published (creator crashed between ftruncate and the magic
// store), the driver initializes it itself instead of refusing forever.
ReclineShm* OpenShm(bool takeOverUninitialized, std::string* err, const char* name = kShmName);

}  // namespace recline
