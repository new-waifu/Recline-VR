// Shared-memory protocol /recline_v1: non-inline parts (snapshot helpers,
// command submit, segment open/create). See recline_shm.h.
#include "recline_shm.h"

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
  for (int i = 0; i < 4; ++i) St(b.R_q[i], v.R_q[i]);
  for (int i = 0; i < 3; ++i) St(b.R_t[i], v.R_t[i]);
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

ReclineShm* OpenShm(bool takeOverUninitialized, std::string* err, const char* name) {
  bool created = false;
  std::string e;
  void* p = MapSharedMemory(name, kShmSize, &created, &e);
  if (!p) {
    if (err) *err = e;
    return nullptr;
  }
  auto* shm = static_cast<ReclineShm*>(p);

  if (created) {  // zero-filled
    St(shm->version, kVersion);
    St(shm->magic, kMagic, std::memory_order_release);
    return shm;
  }

  uint32_t magic = 0;
  for (int i = 0; i < 100; ++i) {  // ~1 s
    magic = Ld(shm->magic, std::memory_order_acquire);
    if (magic == kMagic) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (magic != kMagic) {
    if (magic == 0 && takeOverUninitialized) {
      St(shm->version, kVersion);
      St(shm->magic, kMagic, std::memory_order_release);
      return shm;
    }
    if (err) *err = "shared memory /recline_v1 was never initialized (bad magic)";
    UnmapSharedMemory(p, kShmSize);
    return nullptr;
  }
  const uint32_t ver = Ld(shm->version);
  if (ver != kVersion) {
    if (err) {
      char buf[128];
      snprintf(buf, sizeof buf, "shared memory version %u, expected %u", ver, kVersion);
      *err = buf;
    }
    UnmapSharedMemory(p, kShmSize);
    return nullptr;
  }
  return shm;
}

}  // namespace recline
