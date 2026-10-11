// Shared-memory protocol /recline_v1 between driver_recline and its clients
// (recline_overlay, reclinectl). See docs/DESIGN.md, "Shared-memory protocol".
//
// Every block is a seqlock. All accesses to the seq word *and* the payload go
// through the __atomic builtins (payload: relaxed; ordering comes from the
// seq accesses and explicit fences), so there is no data race even on torn
// reads. Doubles are accessed with the generic __atomic_load/__atomic_store
// builtins, which are plain lock-free 64-bit loads/stores on aarch64/x86-64.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

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
  FLAG_CONFLICT_WRAPPED = 1u << 3,
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
static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0), "u32 atomics must be lock-free");
static_assert(__atomic_always_lock_free(sizeof(uint64_t), 0), "u64 atomics must be lock-free");
static_assert(__atomic_always_lock_free(sizeof(double), 0), "double atomics must be lock-free");

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
inline T Ld(const T& r) {  // relaxed payload load
  T v;
  __atomic_load(&r, &v, __ATOMIC_RELAXED);
  return v;
}
template <class T>
inline void St(T& r, T v) {  // relaxed payload store
  __atomic_store(&r, &v, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// Seqlock protocol

// Reader. `copy` copies every payload field into locals with Ld(). Returns
// false if no consistent snapshot was obtained within maxAttempts.
template <class CopyFn>
inline bool SeqRead(const uint32_t& seq, CopyFn&& copy, int maxAttempts) {
  for (int i = 0; i < maxAttempts; ++i) {
    const uint32_t s1 = __atomic_load_n(&seq, __ATOMIC_ACQUIRE);  // (1)
    if (s1 & 1u) {  // writer active: loop before reading the payload
      CpuRelax();
      continue;
    }
    copy();                                                   // (2)
    __atomic_thread_fence(__ATOMIC_ACQUIRE);                  // (3)
    const uint32_t s2 = __atomic_load_n(&seq, __ATOMIC_RELAXED);  // (4)
    if (s2 == s1) return true;
    CpuRelax();
  }
  return false;
}

// Single-writer begin: returns the even value to pass to SeqWriteEnd.
inline uint32_t SeqWriteBeginSingle(uint32_t& seq) {
  const uint32_t s = __atomic_load_n(&seq, __ATOMIC_RELAXED);  // only we write it: even
  __atomic_store_n(&seq, s + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);  // odd store before payload stores
  return s;
}

// Multi-writer try-lock, one pass: fails immediately if another writer holds
// the block. Used by the hot path, which must never spin.
inline bool SeqTryLock(uint32_t& seq, uint32_t* out) {
  uint32_t s = __atomic_load_n(&seq, __ATOMIC_RELAXED);
  for (;;) {
    if (s & 1u) return false;
    // On failure `s` is reloaded; loop re-checks parity (spurious failures of
    // the weak CAS are retried, real contention shows up as odd).
    if (__atomic_compare_exchange_n(&seq, &s, s + 1, /*weak=*/true, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      __atomic_thread_fence(__ATOMIC_RELEASE);
      *out = s;
      return true;
    }
  }
}

inline void SeqWriteEnd(uint32_t& seq, uint32_t s) {
  __atomic_store_n(&seq, s + 2, __ATOMIC_RELEASE);  // payload stores happen-before this
}

// ---------------------------------------------------------------------------
// Helpers

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

inline bool ReadCmd(const CmdBlock& b, CmdSnapshot* o, int attempts = 1000) {
  return SeqRead(b.seq, [&] {
    o->cmd_id = Ld(b.cmd_id);
    o->cmd = Ld(b.cmd);
    for (int i = 0; i < 4; ++i) o->q[i] = Ld(b.q[i]);
    for (int i = 0; i < 3; ++i) o->t[i] = Ld(b.t[i]);
  }, attempts);
}
inline bool ReadState(const StateBlock& b, StateSnapshot* o, int attempts = 100000) {
  return SeqRead(b.seq, [&] {
    o->cmd_ack = Ld(b.cmd_ack);
    o->cmd_result = Ld(b.cmd_result);
    o->flags = Ld(b.flags);
    o->hmd_index = Ld(b.hmd_index);
    for (int i = 0; i < 4; ++i) o->R_q[i] = Ld(b.R_q[i]);
    for (int i = 0; i < 3; ++i) o->R_t[i] = Ld(b.R_t[i]);
  }, attempts);
}
inline bool ReadHmd(const HmdBlock& b, HmdSnapshot* o, int attempts = 100000) {
  return SeqRead(b.seq, [&] {
    for (int i = 0; i < 4; ++i) o->q[i] = Ld(b.q[i]);
    for (int i = 0; i < 3; ++i) o->t[i] = Ld(b.t[i]);
    o->time = Ld(b.time);
    o->pose_count = Ld(b.pose_count);
  }, attempts);
}

// Single writer (driver command thread).
inline void WriteState(StateBlock& b, const StateSnapshot& v) {
  const uint32_t s = SeqWriteBeginSingle(b.seq);
  St(b.cmd_ack, v.cmd_ack);
  St(b.cmd_result, v.cmd_result);
  St(b.flags, v.flags);
  St(b.hmd_index, v.hmd_index);
  for (int i = 0; i < 4; ++i) St(b.R_q[i], v.R_q[i]);
  for (int i = 0; i < 3; ++i) St(b.R_t[i], v.R_t[i]);
  SeqWriteEnd(b.seq, s);
}

// Multi-writer command submit (overlay, reclinectl). Spins/yields while
// another writer holds the block. The driver force-releases a block left odd
// for > 1 s by a crashed writer, so waiting up to timeoutSec (> 1 s) recovers.
// Returns the cmd_id written, or 0 on timeout.
inline uint32_t SubmitCmd(CmdBlock& b, uint32_t cmd, const double q[4], const double t[3],
                          double timeoutSec = 2.0) {
  const double deadline = MonotonicSeconds() + timeoutSec;
  uint32_t s;
  int spins = 0;
  while (!SeqTryLock(b.seq, &s)) {
    if (++spins < 64) {
      CpuRelax();
    } else {
      sched_yield();
      if (MonotonicSeconds() > deadline) return 0;
    }
  }
  uint32_t id = Ld(b.cmd_id) + 1;
  if (id == 0) id = 1;  // 0 means "never"
  St(b.cmd_id, id);
  St(b.cmd, cmd);
  for (int i = 0; i < 4; ++i) St(b.q[i], q ? q[i] : (i == 0 ? 1.0 : 0.0));
  for (int i = 0; i < 3; ++i) St(b.t[i], t ? t[i] : 0.0);
  SeqWriteEnd(b.seq, s);
  return id;
}

// ---------------------------------------------------------------------------
// Open / create: the creator publishes `magic` last with release ordering;
// openers wait briefly for it and refuse a version mismatch.
//
// takeOverUninitialized: the driver passes true. If an existing segment never
// gets its magic published (creator crashed between ftruncate and the magic
// store), the driver initializes it itself instead of refusing forever.
inline ReclineShm* OpenShm(bool takeOverUninitialized, std::string* err,
                           const char* name = kShmName) {
  bool created = false;
  int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd >= 0) {
    created = true;
    if (ftruncate(fd, kShmSize) != 0) {
      if (err) *err = std::string("ftruncate: ") + strerror(errno);
      close(fd);
      shm_unlink(name);
      return nullptr;
    }
  } else if (errno == EEXIST) {
    fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
      if (err) *err = std::string("shm_open existing: ") + strerror(errno);
      return nullptr;
    }
    struct stat st {};
    // A concurrent creator may not have run ftruncate yet: wait briefly.
    for (int i = 0; i < 100; ++i) {
      if (fstat(fd, &st) == 0 && size_t(st.st_size) >= kShmSize) break;
      usleep(10000);
    }
    if (size_t(st.st_size) < kShmSize) {
      if (err) *err = "existing /dev/shm/recline_v1 has the wrong size";
      close(fd);
      return nullptr;
    }
  } else {
    if (err) *err = std::string("shm_open: ") + strerror(errno);
    return nullptr;
  }

  void* p = mmap(nullptr, kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (p == MAP_FAILED) {
    if (err) *err = std::string("mmap: ") + strerror(errno);
    return nullptr;
  }
  auto* shm = static_cast<ReclineShm*>(p);

  if (created) {  // zero-filled by ftruncate
    __atomic_store_n(&shm->version, kVersion, __ATOMIC_RELAXED);
    __atomic_store_n(&shm->magic, kMagic, __ATOMIC_RELEASE);
    return shm;
  }

  uint32_t magic = 0;
  for (int i = 0; i < 100; ++i) {  // ~1 s
    magic = __atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE);
    if (magic == kMagic) break;
    usleep(10000);
  }
  if (magic != kMagic) {
    if (magic == 0 && takeOverUninitialized) {
      __atomic_store_n(&shm->version, kVersion, __ATOMIC_RELAXED);
      __atomic_store_n(&shm->magic, kMagic, __ATOMIC_RELEASE);
      return shm;
    }
    if (err) *err = "shared memory /recline_v1 was never initialized (bad magic)";
    munmap(p, kShmSize);
    return nullptr;
  }
  const uint32_t ver = __atomic_load_n(&shm->version, __ATOMIC_RELAXED);
  if (ver != kVersion) {
    if (err) {
      char buf[128];
      snprintf(buf, sizeof buf, "shared memory version %u, expected %u", ver, kVersion);
      *err = buf;
    }
    munmap(p, kShmSize);
    return nullptr;
  }
  return shm;
}

}  // namespace recline
