#include <cinttypes>
#include <optional>
#include <iostream>
#include <chrono>
#include <vector>
#include <string>
#include <fstream>

#include <thread>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include "vspace_glue.h"
#include "nr.h"
#include "thread_pin.h"

#ifdef __clang__
extern "C" {
#endif

#include "mcs.h"
#include "aqs.h"

#ifdef __clang__
}
#endif

class key_generator {
  uint64_t state;

  // key range is: `VSPACE_RANGE` as defined in vspace/lib.rs
  // adjust this number together with `MASK` (= VSPACE_RANGE & !0xfff):
  //
  //static constexpr uint64_t MASK = 0x3fffffffff & ~0xfff; // 256 GiB
  static constexpr uint64_t MASK = 0x7fffffffff & ~0xfff; // 512 GiB
  

public:
  key_generator(uint8_t thread_id)
    : state{0xdeadbeefdeadbeef ^ thread_id}
  {}

  // https://en.wikipedia.org/wiki/Xorshift
  uint64_t next() {
    uint64_t x = state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state = x;
    return x & MASK;
  }
};

using seconds = std::chrono::seconds;

struct benchmark_state {
  std::string bench_name;
  size_t n_threads;
  size_t reads_pct;
  size_t reads_stride;
  size_t updates_stride;
  seconds run_seconds;
  core_map& cores;
  std::string run_id;
  std::atomic<size_t> n_threads_ready;
  std::vector<std::thread> threads;
  std::atomic<bool> start_benchmark;
  std::atomic<bool> exit_benchmark;
  std::atomic<size_t> n_threads_finished;
  std::atomic<uint64_t> total_updates;
  std::atomic<uint64_t> total_reads;

  static constexpr size_t stride1 = 10000;

  benchmark_state(std::string& bench_name,
                  size_t n_threads,
                  size_t reads_pct,
                  seconds run_seconds,
                  core_map& cores,
                  std::string run_id)
    : bench_name{bench_name}
    , n_threads{n_threads}
    , reads_pct{reads_pct}
    , reads_stride{reads_pct != 0 ? stride1 * 100 / reads_pct : ~0lu}
    , updates_stride{reads_pct == 100 ? ~0lu: stride1 * 100 / (100 - reads_pct)}
    , run_seconds{run_seconds}
    , cores{cores}
    , run_id{run_id}
    , n_threads_ready{}
    , threads{}
    , start_benchmark{}
    , exit_benchmark{}
    , n_threads_finished{}
    , total_updates{}
    , total_reads{}
  {}

  void dump_json() {
    std::string outpath{"data-"};
    outpath += bench_name + "-";
    outpath += std::to_string(n_threads) + "-";
    outpath += std::to_string(reads_pct) + "-";
    outpath += std::to_string(nr_helper::num_replicas()) + "-";
    outpath += std::to_string(run_seconds.count()) + "-";
    outpath += cores.get_numa_policy() == core_map::NUMA_INTERLEAVE ?
                    "interleave" : "fill";
    if (run_id != "")
      outpath += "-" + run_id;
    outpath += ".json";
    std::ofstream out{outpath.c_str()};

    out << "{" << std::endl
        << "  \"bench_name\": \"" << bench_name << "\"," << std::endl
        << "  \"n_threads\": " << n_threads << "," << std::endl
        << "  \"reads_pct\": " << reads_pct << "," << std::endl
        << "  \"n_replicas\": " << nr_helper::num_replicas() << "," << std::endl
        << "  \"run_seconds\": " << run_seconds.count() << "," << std::endl
        << "  \"numa_policy\": " << cores.get_numa_policy() << "," << std::endl
        << "  \"core_policy\": " << cores.get_core_policy() << "," << std::endl
        << "  \"reads\": " << total_reads << "," << std::endl
        << "  \"updates\": " << total_updates << "," << std::endl
        << "  \"total_ops\": " << total_reads + total_updates << "," << std::endl
        << "  \"reads_per_s\": "
          << static_cast<double>(total_reads) / run_seconds.count()
          << "," << std::endl
        << "  \"updates_per_s\": "
          << static_cast<double>(total_updates) / run_seconds.count()
          << "," << std::endl
        << "  \"ops_per_s\": "
          << static_cast<double>(total_reads + total_updates) / run_seconds.count()
          << "," << std::endl
        << "}" << std::endl;
  }
};

template <typename Monitor>
void run_thread(
    uint8_t thread_id,
    benchmark_state& state,
    Monitor& monitor)
{
  uint32_t core_id = state.cores.pin(thread_id);

  key_generator keygen{thread_id};
  void* thread_context = monitor.create_thread_context(thread_id, core_id);

  state.n_threads_ready++;
  while (!state.start_benchmark) {}

  uint64_t updates = 0;
  uint64_t reads = 0;
  uint64_t updates_vruntime = 0;
  uint64_t reads_vruntime = 0;
  while (!state.exit_benchmark.load(std::memory_order_relaxed)) {
    for (uint32_t i = 0; i < 32; ++i) {
      uint64_t key = keygen.next();
      uint64_t val = key;
      if (reads_vruntime <= updates_vruntime) {
        monitor.read(thread_id, core_id, thread_context, key);
        ++reads;
        reads_vruntime += state.reads_stride;
      } else { // do an update
        monitor.update(thread_id, core_id, thread_context, key, val);
        ++updates;
        updates_vruntime += state.updates_stride;
      }
    }
  }

  state.n_threads_finished++;
  while (state.n_threads_finished < state.n_threads)
    monitor.finish_up(thread_id, core_id, thread_context);

  state.total_updates += updates;
  state.total_reads += reads;
}

// - C++ shared_mutex Benchmarking -

struct cpp_shared_mutex_monitor {
  using s_lock = std::shared_lock<std::shared_mutex>;
  using x_lock = std::unique_lock<std::shared_mutex>;

  std::shared_mutex mutex;
#if USE_COUNTER
  uint64_t value;
#else
  ::VSpacePtr vspace;
#endif

  cpp_shared_mutex_monitor(size_t n_threads)
    : mutex{}
#if USE_COUNTER
    , value{}
#else
    , vspace{createVSpace()}
#endif
  {}

  ~cpp_shared_mutex_monitor() {
  #if USE_COUNTER
  #else
    //delete vspace; // TODO(stutsman): ~VSpace is deleted for some reason?
  #endif
  }

  void* create_thread_context(uint8_t thread_id, uint32_t core_id) { return nullptr; }

  uint64_t read(uint8_t thread_id, uint32_t core_id, void* thread_context, uint64_t key) {
  #if USE_COUNTER
    s_lock lock{mutex};
    return value;
  #else
    s_lock lock{mutex};
    return vspace->resolveWrapped(key);
  #endif
  }

  void update(uint8_t thread_id, uint32_t core_id, void* thread_context, uint64_t key, uint64_t value) {
  #if USE_COUNTER
    x_lock lock{mutex};
    ++value;
  #else
    x_lock lock{mutex};
    vspace->mapGenericWrapped(key, key, 4096);
  #endif
  }

  void finish_up(uint8_t thread_id, uint32_t core_id, void* thread_context) {}
};

// - C MCS Lock Benchmarking -

struct mcs_monitor {
  mcs_mutex_t* mutex;
#if USE_COUNTER
  uint64_t value;
#else
  ::VSpacePtr vspace;
#endif

  mcs_monitor(size_t n_threads)
    : mutex{mcs_mutex_create(NULL)}
#if USE_COUNTER
    , value{}
#else
    , vspace{createVSpace()}
#endif
  {}

  ~mcs_monitor() {
#if USE_COUNTER
#else
    //delete vspace; // TODO(stutsman): ~VSpace is deleted for some reason?
#endif
    mcs_mutex_destroy(mutex);
  }

  void* create_thread_context(uint8_t thread_id, uint32_t core_id) {
    return new mcs_node_t{}; // Just leak it...
  }

  uint64_t read(uint8_t thread_id, uint32_t core_id, void* thread_context, uint64_t key) {
    mcs_node_t* me = static_cast<mcs_node_t*>(thread_context);
    uint64_t r;
    mcs_mutex_lock(mutex, me);
  #if USE_COUNTER
    r = value;
  #else
    r = vspace->resolveWrapped(key);
  #endif
    mcs_mutex_unlock(mutex, me);
    return r;
  }

  void update(uint8_t thread_id, uint32_t core_id, void* thread_context, uint64_t key, uint64_t value) {
    mcs_node_t* me = static_cast<mcs_node_t*>(thread_context);
    mcs_mutex_lock(mutex, me);
  #if USE_COUNTER
    ++value;
  #else
    vspace->mapGenericWrapped(key, key, 4096);
  #endif
    mcs_mutex_unlock(mutex, me);
  }

  void finish_up(uint8_t thread_id, uint32_t core_id, void* thread_context) {}
};

// - ShflLock Benchmarking -

struct shfllock_monitor {
  aqs_mutex_t* mutex;
#if USE_COUNTER
  uint64_t value;
#else
  ::VSpacePtr vspace;
#endif

  shfllock_monitor(size_t n_threads)
    : mutex{aqs_mutex_create(NULL)}
#if USE_COUNTER
    , value{}
#else
    , vspace{createVSpace()}
#endif
  {}

  ~shfllock_monitor() {
#if USE_COUNTER
#else
    //delete vspace; // TODO(stutsman): ~VSpace is deleted for some reason?
#endif
    aqs_mutex_destroy(mutex);
  }

  void* create_thread_context(uint8_t thread_id, uint32_t core_id) {
    aqs_mutex_set_cur_thread_id(core_id);
    return new aqs_node_t{}; // Just leak it...
  }

  uint64_t read(uint8_t thread_id, uint32_t core_id, void* thread_context, uint64_t key) {
    aqs_node_t* me = static_cast<aqs_node_t*>(thread_context);
    uint64_t r;
    aqs_mutex_lock(mutex, me);
  #if USE_COUNTER
    r = value;
  #else
    r = vspace->resolveWrapped(key);
  #endif
    aqs_mutex_unlock(mutex, me);
    return r;
  }

  void update(uint8_t thread_id, uint32_t core_id, void* thread_context, uint64_t key, uint64_t value) {
    aqs_node_t* me = static_cast<aqs_node_t*>(thread_context);
    aqs_mutex_lock(mutex, me);
  #if USE_COUNTER
    ++value;
  #else
    vspace->mapGenericWrapped(key, key, 4096);
  #endif
    aqs_mutex_unlock(mutex, me);
  }

  void finish_up(uint8_t thread_id, uint32_t core_id, void* thread_context) {}
};

// - RwLock Benchmarking -

// Give a friendlier name to Dafny's generated namespace.
#if USE_COUNTER
namespace rwlock = RwLockImpl_ON_Uint64ContentsTypeMod__Compile;
#else
namespace rwlock = RwLockImpl_ON_VSpaceContentsTypeMod__Compile;
#endif
typedef rwlock::RwLock RwLockT;

struct dafny_rwlock_monitor {
  RwLockT lock;

  dafny_rwlock_monitor(size_t n_threads)
  #if USE_COUNTER
    : lock{rwlock::__default::new__mutex(0lu)}
  #else
    : lock{rwlock::__default::new__mutex(createVSpace())}
  #endif
  {}

  ~dafny_rwlock_monitor() {
  #if USE_COUNTER
  #else
    ::VSpacePtr vspace = lock.acquire();
    // delete vspace; // TODO(stutsman): ~VSpace is deleted for some reason?
    lock.release(nullptr);
  #endif
  }

  void* create_thread_context(uint8_t thread_id, uint32_t core_id) {
    return nullptr;
  }

  uint64_t read(uint8_t thread_id, uint32_t core_id, void* thread_context, uint64_t key) {
  #if USE_COUNTER
    auto shared_guard = lock.acquire__shared(thread_id);
    uint64_t value = *rwlock::__default::borrow__shared(lock, shared_guard);
    lock.release__shared(shared_guard);
    return value;
  #else
    auto shared_guard = lock.acquire__shared(thread_id);
    ::VSpacePtr vspace = *rwlock::__default::borrow__shared(lock, shared_guard);
    uint64_t value = vspace->resolveWrapped(key);
    lock.release__shared(shared_guard);
    return value;
  #endif
  }

  void update(uint8_t thread_id, uint32_t core_id, void* thread_context, uint64_t key, uint64_t value) {
#if USE_COUNTER
    uint64_t val = lock.acquire();
    lock.release(val + 1);
#else
    ::VSpacePtr vspace = lock.acquire();
    bool ok = vspace->mapGenericWrapped(key, value, 4096);
    lock.release(vspace);
#endif
  }

  void finish_up(uint8_t thread_id, uint32_t core_id, void* thread_context) {}
};

// - NR Benchmarking -

struct dafny_nr_monitor{
  nr_helper helper;

  dafny_nr_monitor(size_t n_threads)
    : helper{n_threads}
  {
    helper.init_nr();
  }

  void* create_thread_context(uint8_t thread_id, uint32_t core_id) {
    return helper.register_thread(core_id);
  }

  uint64_t read(uint8_t thread_id, uint32_t core_id, void* context, uint64_t key) {
    auto c = static_cast<nr::ThreadOwnedContext*>(context);
#if USE_COUNTER
    auto op = CounterIfc_Compile::ReadonlyOp{}; 
#else
    auto op = VSpaceIfc_Compile::ReadonlyOp{key}; 
#endif
    Tuple<uint64_t, nr::ThreadOwnedContext> r =
      nr::__default::do__read(
        helper.get_nr(),
        *helper.get_node(core_id),
        op,
        *c);

    return r.get<0>();
  }

  void update(uint8_t thread_id, uint32_t core_id, void* context, uint64_t key, uint64_t value) {
    auto c = static_cast<nr::ThreadOwnedContext*>(context);
#if USE_COUNTER
    auto op = CounterIfc_Compile::UpdateOp{}; 
#else
    auto op = VSpaceIfc_Compile::UpdateOp{key, value}; 
#endif
    nr::__default::do__update(
      helper.get_nr(),
      *helper.get_node(core_id),
      op,
      *c);
  }

  void finish_up(uint8_t thread_id, uint32_t core_id, void* context) {
    auto c = static_cast<nr::ThreadOwnedContext*>(context);
    nr::__default::try__combine(
      helper.get_nr(),
      *helper.get_node(core_id),
      c->tid,
      c->activeIdxs);
  }
};

// - Rust NR Benchmarking -

struct rust_nr_monitor{
  nr_rust_helper helper;

  rust_nr_monitor(size_t n_threads)
    : helper{n_threads}
  {
    helper.init_nr();
  }

  void* create_thread_context(uint8_t thread_id, uint32_t core_id) {
    return (void*)helper.register_thread(core_id);
  }

  uint64_t read(uint8_t thread_id, uint32_t core_id, void* context, uint64_t key) {
    size_t replica_token = (size_t) context;
#if USE_COUNTER
    #error "NYI"
#else
    helper.get_node(core_id)->ReplicaResolve(replica_token, key);
#endif
    return 0;
  }

  void update(uint8_t thread_id, uint32_t core_id, void* context, uint64_t key, uint64_t value) {
    size_t replica_token = (size_t)context;
#if USE_COUNTER
  #error "NYI"
#else
    bool ok = helper.get_node(core_id)->ReplicaMap(replica_token, key, value);
#endif
  }

  void finish_up(uint8_t thread_id, uint32_t core_id, void* context) {
    auto replica_token = (size_t)context;
    helper.get_node(core_id)->ReplicaResolve(replica_token, 0x0);
  }
};

template <typename Monitor>
void bench(benchmark_state& state, Monitor& monitor)
{
  for (uint8_t thread_id = 0; thread_id < state.n_threads; ++thread_id) {
    state.threads.emplace_back(std::thread{run_thread<Monitor>,
                                           thread_id,
                                           std::ref(state),
                                           std::ref(monitor)});
  }

  while (state.n_threads_ready < state.n_threads);
  state.start_benchmark = true;
  std::this_thread::sleep_for(state.run_seconds);
  state.exit_benchmark = true;

  for (auto& thread : state.threads)
    thread.join();

  const size_t total_ops = state.total_updates + state.total_reads;
  std::cerr << std::endl
            << "threads " << state.n_threads << std::endl
            << "updates " << state.total_updates << std::endl
            << "reads   " << state.total_reads << std::endl
            << "Mops    " << total_ops / 1e6 << std::endl
            << "Mops/s  " << total_ops / 1e6 / state.run_seconds.count()
            << std::endl;

  state.dump_json();
}

void usage(const char* argv0) {
  std::cerr << "usage: " << argv0
            << " <benchmarkname> <n_threads> <read_pct>"
            << " <n_seconds> <fill|interleave> [run_id]"
            << std::endl;
  exit(-1);
}

int main(int argc, char* argv[]) {
  if (argc < 6)
    usage(argv[0]);

  //LogWrapper& lw = createLog();
  //ReplicaWrapper* rw = createReplica(lw);
  //auto tkn = rw->RegisterWrapper();
  //rw->ReplicaMap(tkn, 0x2000, 0x3000);
  //rw->ReplicaResolve(tkn, 0x2000);

  //VSpace* vspace = createVSpace();
  //vspace->mapGenericWrapped(0x2000, 0x3000, 0x1000);

  std::string bench_name = std::string{argv[1]};

  const size_t n_threads = atoi(argv[2]);
  assert(n_threads > 0);

  const size_t reads_pct = atoi(argv[3]);
  assert(reads_pct <= 100);

  const size_t n_seconds = atoi(argv[4]);
  assert(n_seconds > 0);
  const auto run_seconds = std::chrono::seconds{n_seconds};

  core_map::numa_policy fill_policy;
  const std::string policy_name = std::string{argv[5]};
  if (policy_name == "fill") {
    fill_policy = core_map::NUMA_FILL;
  } else if (policy_name == "interleave") {
    fill_policy = core_map::NUMA_INTERLEAVE;
  } else {
    usage(argv[0]);
  }

  std::string run_id{};
  if (argc == 7)
    run_id = argv[6];

  disable_dvfs();

  core_map cores{fill_policy, core_map::core_policy::CORES_FILL};

  // It's possible to run with 4 Dafny NR nodes, but then only create
  // enough threads such that, say, threads only run on 3 of the 4
  // NUMA nodes. In this case, things would deadlock since no threads
  // drive the 4th replica. Just bail if we run into a configuration
  // that would suffer from that problem.
  //assert(!(bench_name == "dafny_nr" || bench_name == "rust_nr") ||
         //cores.n_active_nodes(n_threads, &nr_helper::get_node_id) == nr_helper::num_replicas());

  // The main thread mainly just sleeps, but pin it before constructing
  // all the benchmark state to ensure determinism wrt to first-touch policy.
  cores.pin(0);

  benchmark_state state{
    bench_name,
    n_threads,
    reads_pct,
    run_seconds,
    cores,
    run_id
  };

#define BENCHMARK(test_name) \
  if (bench_name == #test_name) { \
    test_name ## _monitor monitor{n_threads}; \
    bench(state, monitor); \
    exit(0); \
  }

  BENCHMARK(cpp_shared_mutex);
  BENCHMARK(dafny_rwlock);
  BENCHMARK(dafny_nr);
  BENCHMARK(rust_nr);
  BENCHMARK(mcs);
  BENCHMARK(shfllock);

  std::cerr << "unrecognized benchmark name " << bench_name << std::endl;

  return -1;
}


