// Shared-memory protocol /recline_v1: non-inline parts (snapshot helpers,
// command submit, segment open/create). See recline_shm.h.
#include "recline_shm.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace recline {

bool ReadCmd(const CmdBlock& b, CmdSnapshot* o, int attempts) {
  return SeqRead(b.seq, [&] {
    o->cmd_id = Ld(b.cmd_id);
    o->cmd = Ld(b.cmd);
    for (int i = 0; i < 4; ++i) o->q[i] = Ld(b.q[i]);
    for (int i = 0; i < 3; ++i) o->t[i] = Ld(b.t[i]);
  }, attempts);
}
bool ReadState(const StateBlock& b, StateSnapshot* o, int attempts) {
  return SeqRead(b.seq, [&] {
    o->cmd_ack = Ld(b.cmd_ack);
    o->cmd_result = Ld(b.cmd_result);
    o->flags = Ld(b.flags);
    o->hmd_index = Ld(b.hmd_index);
    o->auto_reset = Ld(b.auto_reset);
    for (int i = 0; i < 4; ++i) o->R_q[i] = Ld(b.R_q[i]);
    for (int i = 0; i < 3; ++i) o->R_t[i] = Ld(b.R_t[i]);
  }, attempts);
}
bool ReadHmd(const HmdBlock& b, HmdSnapshot* o, int attempts) {
  return SeqRead(b.seq, [&] {
    for (int i = 0; i < 4; ++i) o->q[i] = Ld(b.q[i]);
    for (int i = 0; i < 3; ++i) o->t[i] = Ld(b.t[i]);
    o->time = Ld(b.time);
    o->pose_count = Ld(b.pose_count);
  }, attempts);
}

void WriteState(StateBlock& b, const StateSnapshot& v) {
  const uint32_t s = SeqWriteBeginSingle(b.seq);
  St(b.cmd_ack, v.cmd_ack);
  St(b.cmd_result, v.cmd_result);
  St(b.flags, v.flags);
  St(b.hmd_index, v.hmd_index);
  St(b.auto_reset, v.auto_reset);
  for (int i = 0; i < 4; ++i) St(b.R_q[i], v.R_q[i]);
  for (int i = 0; i < 3; ++i) St(b.R_t[i], v.R_t[i]);
  SeqWriteEnd(b.seq, s);
}

bool ReadIdentity(const ReclineShm& shm, DriverIdentity* o) {
  const IdentityBlock& b = shm.identity;
  uint32_t magic = 0, pid = 0;
  int64_t size = 0, mtime = 0;
  char version[sizeof b.version], path[sizeof b.module_path];
  const bool ok = SeqRead(b.seq, [&] {
    magic = Ld(b.magic);
    pid = Ld(b.pid);
    size = Ld(b.module_size);
    mtime = Ld(b.module_mtime);
    for (size_t i = 0; i < sizeof version; ++i) version[i] = Ld(b.version[i]);
    for (size_t i = 0; i < sizeof path; ++i) path[i] = Ld(b.module_path[i]);
  }, 1000);
  if (!ok) return false;
  version[sizeof version - 1] = path[sizeof path - 1] = 0;
  *o = DriverIdentity{};
  o->layout = Ld(shm.version);
  o->present = magic == kIdentityMagic;
  if (!o->present) return true;
  o->pid = pid;
  o->module_size = size;
  o->module_mtime = mtime;
  o->version = version;
  o->module_path = path;
  return true;
}

namespace {

template <size_t N>
void StoreString(char (&dst)[N], const std::string& s) {
  const size_t n = std::min(s.size(), N - 1);
  for (size_t i = 0; i < N; ++i) St(dst[i], i < n ? s[i] : '\0');
}

// A new or taken-over segment: write the layout version, then publish the magic.
void Initialize(ReclineShm* shm) {
  St(shm->version, kVersion);
  St(shm->magic, kMagic, std::memory_order_release);
}

}  // namespace

void WriteIdentity(IdentityBlock& b, const DriverIdentity& v) {
  const uint32_t s = SeqWriteBeginSingle(b.seq);
  St(b.pid, v.pid);
  St(b.module_size, v.module_size);
  St(b.module_mtime, v.module_mtime);
  StoreString(b.version, v.version);
  StoreString(b.module_path, v.module_path);
  St(b.magic, kIdentityMagic);
  SeqWriteEnd(b.seq, s);
}

uint32_t SubmitCmd(CmdBlock& b, uint32_t cmd, const double q[4], const double t[3],
                   double timeoutSec) {
  const double deadline = MonotonicSeconds() + timeoutSec;
  uint32_t s;
  int spins = 0;
  while (!SeqTryLock(b.seq, &s)) {
    if (++spins < 64) {
      CpuRelax();
    } else {
      std::this_thread::yield();
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

ReclineShm* OpenShm(bool driver, std::string* err, const char* name, DriverIdentity* found) {
  bool created = false, missing = false;
  std::string e;
  void* p = MapSharedMemory(name, kShmSize, /*create=*/driver, &created, &e, &missing);
  if (!p) {
    if (err) *err = missing ? std::string("no driver has created ") + name : e;
    return nullptr;
  }
  auto* shm = static_cast<ReclineShm*>(p);

  if (created) {  // zero-filled
    Initialize(shm);
    return shm;
  }

  uint32_t magic = 0;
  for (int i = 0; i < 100; ++i) {  // ~1 s
    magic = Ld(shm->magic, std::memory_order_acquire);
    if (magic == kMagic) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (magic != kMagic) {
    if (magic == 0 && driver) {
      Initialize(shm);
      return shm;
    }
    if (err) *err = std::string("shared memory ") + name + " was never initialized (bad magic)";
    UnmapSharedMemory(p, kShmSize);
    return nullptr;
  }
  if (found) ReadIdentity(*shm, found);
  const uint32_t ver = Ld(shm->version);
  if (ver == kVersion) return shm;
  UnmapSharedMemory(p, kShmSize);
  if (driver) {
    // Replace it. Where the last unmap frees a segment (Windows), another
    // process still has this one open: refuse until it is gone.
    RemoveSharedMemory(name);
    p = MapSharedMemory(name, kShmSize, /*create=*/true, &created, &e);
    if (p && created) {
      Initialize(static_cast<ReclineShm*>(p));
      return static_cast<ReclineShm*>(p);
    }
    if (p) UnmapSharedMemory(p, kShmSize);
  }
  if (err) {
    char buf[160];
    snprintf(buf, sizeof buf, "shared memory %s has layout version %u, expected %u%s", name, ver,
             kVersion, driver ? " (still open in another process)" : "");
    *err = buf;
  }
  return nullptr;
}

}  // namespace recline
