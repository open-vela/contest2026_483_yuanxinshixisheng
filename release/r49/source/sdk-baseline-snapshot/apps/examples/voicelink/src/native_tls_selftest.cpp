#include <pthread.h>
#include <sched.h>

#include <cstdint>
#include <cstdio>

namespace {

unsigned destroyed;

struct Value {
  int number = 7;
  ~Value() { __atomic_fetch_add(&destroyed, 1u, __ATOMIC_RELAXED); }
};

Value& local() {
  static thread_local Value value;
  return value;
}

struct MainValueReset {
  ~MainValueReset() { local().number = 7; }
};

void* worker(void* argument) {
  const int number = static_cast<int>(reinterpret_cast<intptr_t>(argument));
  if (local().number != 7) return reinterpret_cast<void*>(1);
  local().number = number;
  for (unsigned i = 0; i < 100; ++i) {
    sched_yield();
    if (local().number != number) return reinterpret_cast<void*>(2);
  }
  return nullptr;
}

}  // namespace

extern "C" int k7_tls_selftest() {
  if (local().number != 7) {
    std::puts("TLS_CHECK FAIL stale initial value");
    return 1;
  }
  MainValueReset reset;
  const unsigned before = __atomic_load_n(&destroyed, __ATOMIC_RELAXED);
  local().number = 123;
  pthread_t first{};
  pthread_t second{};
  if (pthread_create(&first, nullptr, worker, reinterpret_cast<void*>(101)))
    return 1;
  if (pthread_create(&second, nullptr, worker, reinterpret_cast<void*>(102))) {
    pthread_join(first, nullptr);
    return 1;
  }
  void* first_result = nullptr;
  void* second_result = nullptr;
  const int first_join = pthread_join(first, &first_result);
  const int second_join = pthread_join(second, &second_result);
  const unsigned delta =
      __atomic_load_n(&destroyed, __ATOMIC_RELAXED) - before;
  const bool ok = !first_join && !second_join && !first_result &&
                  !second_result && local().number == 123 && delta == 2;
  std::printf("TLS_CHECK %s workers=2 destructors=%u main_value=%d\n",
              ok ? "PASS" : "FAIL", delta, local().number);
  return ok ? 0 : 1;
}
