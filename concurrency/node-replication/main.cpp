#include <cinttypes>
#include <optional>
#include <iostream>
#include <chrono>
#include <vector>

#include <thread>
#include <mutex>
#include <condition_variable>

#include "nr.h"

// - RwLock Benchmarking -

// Give a friendlier name to Dafny's generated namespace.
namespace rwlock = RwLockImpl_ON_Uint64ContentsTypeMod__Compile;
typedef rwlock::RwLock RwLockUint64;

std::atomic<size_t> n_threads_ready{0};
std::atomic<bool> start_benchmark{false};
std::atomic<bool> exit_benchmark{false};

void run_rwlock_bench(
    uint8_t thread_id,
    RwLockUint64& rwlock,
    std::atomic<uint64_t>& total_updates,
    std::atomic<uint64_t>& total_reads)
{
  n_threads_ready++;
  while (!start_benchmark) {}

  uint64_t updates = 0;
  uint64_t reads = 0;
  while (!exit_benchmark.load(std::memory_order_relaxed)) {
    if ((reads + updates) & 0xf) { // do a read
      auto shared_guard = rwlock.acquire__shared(thread_id);
      uint64_t* value = rwlock::__default::borrow__shared(rwlock, shared_guard);
      rwlock.release__shared(shared_guard);
      ++reads;
    } else { // do a write
      bool value = rwlock.acquire();
      rwlock.release(value + 1);
      ++updates;
    }
  }

  total_updates += updates;
  total_reads += reads;
}

template <typename Duration>
void bench_rwlock(size_t n_threads, Duration run_duration) {
  auto rwlock = rwlock::__default::new__mutex(false);
  std::atomic<uint64_t> total_updates{0};
  std::atomic<uint64_t> total_reads{0};

  std::vector<std::thread> threads{};
  for (uint8_t thread_id = 0; thread_id < n_threads; ++thread_id)
    threads.emplace_back(std::thread{run_rwlock_bench,
                                     thread_id,
                                     std::ref(rwlock),
                                     std::ref(total_updates),
                                     std::ref(total_reads)});

  while (n_threads_ready < n_threads);
  start_benchmark = true;
  std::this_thread::sleep_for(run_duration);
  exit_benchmark = true;

  for (auto& thread : threads)
    thread.join();

  std::cout << std::endl
            << "threads " << n_threads << std::endl
            << "updates " << total_updates << std::endl
            << "reads   " << total_reads << std::endl;
}

template <typename Duration>
void bench_nr(size_t n_threads, Duration duration);

int main(int argc, char* argv[]) {
  const auto run_duration = std::chrono::seconds{1};

  if (argc < 2) {
    std::cerr << "usage: " << argv[0] << " <rwlock|nr>" << std::endl;
    exit(-1);
  }

  if (std::string{argv[1]} == "rwlock") {
    bench_rwlock(NUM_THREADS, run_duration);
  } else {
    bench_nr(NUM_THREADS, run_duration);
  }

  return 0;
}

// - NR-related stuff -

void run_nr_bench(
    uint8_t thread_id,
    nr_helper& nr_helper,
    std::atomic<uint64_t>& total_updates,
    std::atomic<uint64_t>& total_reads)
{
  // TODO(stutsman): pin threads properly

  nr::ThreadOwnedContext* context = nr_helper.register_thread(thread_id);

  n_threads_ready++;
  while (!start_benchmark) {}

  // Run one initial read to make sure the count is correct.
  Tuple<uint64_t, nr::ThreadOwnedContext> r =
    Impl_ON_CounterIfc__Compile::__default::do__read(
      nr_helper.get_nr(),
      nr_helper.get_node(thread_id),
      CounterIfc_Compile::ReadonlyOp{},
      *context);
    std::cout << "thread_id final nr initial value: " << r.get<0>() << std::endl;

  uint64_t reads = 0;
  uint64_t updates = 0;
  while (!exit_benchmark.load(std::memory_order_relaxed)) {
    if ((reads + updates) & 0xf) { // do a read
      Tuple<uint64_t, nr::ThreadOwnedContext> r =
        Impl_ON_CounterIfc__Compile::__default::do__read(
          nr_helper.get_nr(),
          nr_helper.get_node(thread_id),
          CounterIfc_Compile::ReadonlyOp{},
          *context);
      ++reads;
    } else { // do a write
      Tuple<uint64_t, nr::ThreadOwnedContext> r =
        Impl_ON_CounterIfc__Compile::__default::do__update(
          nr_helper.get_nr(),
          nr_helper.get_node(thread_id),
          CounterIfc_Compile::UpdateOp{},
          *context);
      ++updates;
    }
  }

  // Run one last read to make sure the final count is correct.
  r =
    Impl_ON_CounterIfc__Compile::__default::do__read(
      nr_helper.get_nr(),
      nr_helper.get_node(thread_id),
      CounterIfc_Compile::ReadonlyOp{},
      *context);
    std::cout << "thread_id final nr counter value: " << r.get<0>() << std::endl;

  total_updates += updates;
  total_reads += reads;
}

template <typename Duration>
void bench_nr(size_t n_threads, Duration run_duration) {
  nr_helper nr_helper{};
  nr_helper.init_nr();

  std::atomic<uint64_t> total_updates{0};
  std::atomic<uint64_t> total_reads{0};
  std::vector<std::thread> threads{};
  for (uint8_t thread_id = 0; thread_id < n_threads; ++thread_id) {
    threads.emplace_back(std::thread{run_nr_bench,
                                     thread_id,
                                     std::ref(nr_helper),
                                     std::ref(total_updates),
                                     std::ref(total_reads)});
  }

  while (n_threads_ready < n_threads);
  start_benchmark = true;
  std::this_thread::sleep_for(run_duration);
  exit_benchmark = true;

  for (auto& thread : threads)
    thread.join();

  std::cout << std::endl
            << "threads " << n_threads << std::endl
            << "updates " << total_updates << std::endl
            << "reads   " << total_reads << std::endl;
}

