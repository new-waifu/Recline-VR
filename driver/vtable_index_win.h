// Vtable index of a virtual member function under MSVC.
//
// MSVC represents a pointer to a virtual member function as the address of a
// vcall thunk that jumps through the object's vtable, so the index is not in the
// pointer. Calling it on a fake object whose vtable entries record their own
// position yields the index.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace recline {

namespace vtable_index_detail {

constexpr size_t kMaxSlots = 64;
inline size_t g_probed = SIZE_MAX;

template <size_t I>
void Probe() {
  g_probed = I;
}
template <size_t... I>
constexpr std::array<void (*)(), sizeof...(I)> MakeProbes(std::index_sequence<I...>) {
  return {{&Probe<I>...}};
}

}  // namespace vtable_index_detail

// SIZE_MAX if the index can't be derived. Not thread-safe: call it from one
// thread at a time (the driver calls it from Init and its command thread).
template <class C, class R, class... A>
size_t VtableIndex(R (C::*pmf)(A...)) {
  using namespace vtable_index_detail;
  static constexpr auto probes = MakeProbes(std::make_index_sequence<kMaxSlots>{});
  struct FakeObject {
    const void* vtbl;
  } fake{probes.data()};
  // Called without arguments: the probes take none and the x64 caller owns the
  // stack, so the mismatch is harmless.
  using AnyFn = void (C::*)();
  const auto any = reinterpret_cast<AnyFn>(pmf);
  g_probed = SIZE_MAX;
  (reinterpret_cast<C*>(&fake)->*any)();
  return g_probed < kMaxSlots ? g_probed : SIZE_MAX;
}

}  // namespace recline
