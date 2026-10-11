// Host stress tests for the /recline_v1 seqlock protocol: readers
// must never observe a torn payload, multi-writer CmdBlock ids must be unique,
// the HMD try-lock must never block, and the open/version handshake must work.
#include <atomic>
#include <cstdio>
#include <set>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "recline_shm.h"

using namespace recline;

static int g_fail = 0;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      ++g_fail;                                                       \
      fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
      fprintf(stderr, __VA_ARGS__);                                   \
      fprintf(stderr, "\n");                                          \
    }                                                                 \
  } while (0)

static void TestStateBlock() {
  alignas(64) static StateBlock b{};
  std::atomic<bool> stop{false};
  std::atomic<long> reads{0}, torn{0};
  std::thread writer([&] {
    for (uint32_t i = 1; i < 300000; ++i) {
      StateSnapshot s;
      s.cmd_ack = i;
      s.cmd_result = i * 3;
      s.flags = i ^ 0x55;
      s.hmd_index = i * 7;
      for (int k = 0; k < 4; ++k) s.R_q[k] = i + k * 0.25;
      for (int k = 0; k < 3; ++k) s.R_t[k] = -double(i) - k;
      WriteState(b, s);
    }
    stop = true;
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r)
    readers.emplace_back([&] {
      while (!stop) {
        StateSnapshot s;
        if (!ReadState(b, &s, 1000)) continue;
        ++reads;
        const uint32_t i = s.cmd_ack;
        bool ok = s.cmd_result == i * 3 && s.flags == (i ^ 0x55) && s.hmd_index == i * 7;
        for (int k = 0; k < 4; ++k) ok = ok && s.R_q[k] == (i ? i + k * 0.25 : (k ? 0 : 0));
        for (int k = 0; k < 3; ++k) ok = ok && s.R_t[k] == (i ? -double(i) - k : 0);
        if (i == 0) ok = true;  // initial zero state
        if (!ok) ++torn;
      }
    });
  writer.join();
  for (auto& t : readers) t.join();
  CHECK(torn == 0, "%ld torn StateBlock reads of %ld", torn.load(), reads.load());
  printf("StateBlock: %ld consistent reads, %ld torn\n", reads.load(), torn.load());
}

static void TestCmdBlockMultiWriter() {
  alignas(64) static CmdBlock b{};
  constexpr int kWriters = 4, kPer = 20000;
  std::atomic<bool> stop{false};
  std::atomic<long> torn{0}, reads{0};
  std::vector<std::vector<uint32_t>> ids(kWriters);
  std::vector<std::thread> ws;
  for (int w = 0; w < kWriters; ++w)
    ws.emplace_back([&, w] {
      for (int i = 0; i < kPer; ++i) {
        // Payload encodes the writer; the reader checks all fields agree.
        const double q[4] = {double(w), double(w) + 0.5, double(w) + 1, double(w) + 1.5};
        const double t[3] = {double(w) * 10, double(w) * 20, double(w) * 30};
        const uint32_t id = SubmitCmd(b, CMD_SET, q, t);
        if (id) ids[w].push_back(id);
      }
    });
  std::thread reader([&] {
    while (!stop) {
      CmdSnapshot c;
      if (!ReadCmd(b, &c, 1000) || c.cmd_id == 0) continue;
      ++reads;
      const double w = c.q[0];
      const bool ok = c.cmd == CMD_SET && c.q[1] == w + 0.5 && c.q[2] == w + 1 &&
                      c.q[3] == w + 1.5 && c.t[0] == w * 10 && c.t[1] == w * 20 &&
                      c.t[2] == w * 30;
      if (!ok) ++torn;
    }
  });
  for (auto& t : ws) t.join();
  stop = true;
  reader.join();
  std::set<uint32_t> all;
  size_t n = 0;
  for (auto& v : ids) {
    for (size_t i = 1; i < v.size(); ++i) CHECK(v[i] > v[i - 1], "ids not increasing per writer");
    all.insert(v.begin(), v.end());
    n += v.size();
  }
  CHECK(n == size_t(kWriters) * kPer, "lost submissions: %zu", n);
  CHECK(all.size() == n, "duplicate cmd ids: %zu unique of %zu", all.size(), n);
  CHECK(*all.rbegin() == n, "final id %u != %zu", *all.rbegin(), n);
  CHECK(torn == 0, "%ld torn CmdBlock reads", torn.load());
  printf("CmdBlock: %zu commands from %d writers, ids unique, %ld reads, %ld torn\n", n, kWriters,
         reads.load(), torn.load());
}

static void TestHmdTryLock() {
  alignas(64) static HmdBlock b{};
  std::atomic<long> published{0}, skipped{0};
  std::vector<std::thread> ws;
  for (int w = 0; w < 4; ++w)
    ws.emplace_back([&, w] {
      for (int i = 0; i < 100000; ++i) {
        uint32_t s;
        if (!SeqTryLock(b.seq, &s)) {
          ++skipped;
          continue;
        }
        for (int k = 0; k < 4; ++k) St(b.q[k], double(w));
        St(b.pose_count, uint64_t(i));
        SeqWriteEnd(b.seq, s);
        ++published;
      }
    });
  for (auto& t : ws) t.join();
  HmdSnapshot h;
  CHECK(ReadHmd(b, &h, 10), "final read");
  CHECK((__atomic_load_n(&b.seq, __ATOMIC_RELAXED) & 1u) == 0, "seq left odd");
  CHECK(h.q[0] == h.q[1] && h.q[1] == h.q[2] && h.q[2] == h.q[3], "torn hmd");
  CHECK(__atomic_load_n(&b.seq, __ATOMIC_RELAXED) == uint32_t(published.load() * 2),
        "seq/published mismatch");
  printf("HmdBlock: %ld published, %ld skipped (never blocked)\n", published.load(),
         skipped.load());
}

static void TestBoundedReadOnStuckWriter() {
  alignas(64) static StateBlock b{};
  __atomic_store_n(&b.seq, 1u, __ATOMIC_RELAXED);  // writer died mid-write
  StateSnapshot s;
  CHECK(!ReadState(b, &s, 16), "bounded read must give up on odd seq");
  alignas(64) static CmdBlock c{};
  __atomic_store_n(&c.seq, 5u, __ATOMIC_RELAXED);
  const double t0 = MonotonicSeconds();
  CHECK(SubmitCmd(c, CMD_RESET, nullptr, nullptr, 0.2) == 0, "submit must time out");
  CHECK(MonotonicSeconds() - t0 < 1.0, "submit timeout bounded");
}

static void TestOpen() {
  char name[64];
  snprintf(name, sizeof name, "/recline_test_%d", int(getpid()));
  shm_unlink(name);
  std::string err;
  ReclineShm* a = OpenShm(false, &err, name);
  CHECK(a != nullptr, "create: %s", err.c_str());
  ReclineShm* b = OpenShm(false, &err, name);
  CHECK(b != nullptr && b != a, "open existing: %s", err.c_str());
  if (a && b) {
    CHECK(b->magic == kMagic && b->version == kVersion, "header");
    const uint32_t id = SubmitCmd(a->cmd, CMD_RESET, nullptr, nullptr);
    CmdSnapshot c;
    CHECK(ReadCmd(b->cmd, &c) && c.cmd_id == id && c.cmd == CMD_RESET, "cross-mapping");
    __atomic_store_n(&a->version, 2u, __ATOMIC_RELAXED);
    CHECK(OpenShm(false, &err, name) == nullptr, "version mismatch must refuse");
    printf("version mismatch refused: %s\n", err.c_str());
    // Uninitialized segment: clients refuse, the driver takes over.
    __atomic_store_n(&a->magic, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&a->version, 0u, __ATOMIC_RELAXED);
    CHECK(OpenShm(false, &err, name) == nullptr, "client must refuse uninitialized");
    ReclineShm* d = OpenShm(true, &err, name);
    CHECK(d && d->magic == kMagic && d->version == kVersion, "driver takeover: %s", err.c_str());
  }
  shm_unlink(name);
}

int main() {
  TestStateBlock();
  TestCmdBlockMultiWriter();
  TestHmdTryLock();
  TestBoundedReadOnStuckWriter();
  TestOpen();
  if (g_fail) {
    fprintf(stderr, "%d failure(s)\n", g_fail);
    return 1;
  }
  printf("test_shm: all passed\n");
  return 0;
}
