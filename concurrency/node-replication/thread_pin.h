#ifndef THREAD_PIN_H
#define THREAD_PIN_H

#include <cinttypes>
#include <memory>

#include <numa.h>
#include <sys/sysinfo.h>

void disable_dvfs() {
  int ret = system("test -d '/sys/devices/system/cpu/cpu0/cpufreq' && "
      "(echo performance | "
      "sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor) > "
      "/dev/null");
  if (ret == -1) {
    std::cerr << "Unable to disable DVFS" << std::endl;
    exit(-1);
  }
}

void enable_dvfs() {
  int ret = system("echo powersave | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor > /dev/null");
  if (ret == -1) {
    std::cerr << "Unable to enable DVFS" << std::endl;
    exit(-1);
  }
}

class core_map {
 public:
  enum numa_policy { NUMA_FILL, NUMA_INTERLEAVE };
  enum core_policy { CORES_FILL, CORES_INTERLEAVE };

 private:
  numa_policy nm_policy;
  core_policy cr_policy;
  typedef uint32_t core_id;
  std::vector<core_id> thread_to_core_map;
  std::vector<uint32_t> core_to_node_map;

 public:
  uint32_t get_node_for_core(core_id core) {
    return core_to_node_map.at(core);
  }

  core_map(numa_policy nm_policy, core_policy cr_policy)
    : nm_policy{nm_policy}
    , cr_policy{cr_policy}
    , thread_to_core_map{}
    , core_to_node_map{}
  {
    if (numa_available() == -1) {
      std::cerr << "NUMA not available" << std::endl;
      exit(-1);
    }

    uint32_t n_cores = numa_num_task_cpus();
    uint32_t n_nodes = numa_num_task_nodes();

    std::vector<std::unique_ptr<bitmask>> nodecpus{};
    nodecpus.resize(n_nodes);

    for (uint32_t i = 0; i < n_nodes; i++) {
      nodecpus[i].reset(numa_allocate_cpumask());
      if (nodecpus[i] == NULL) {
        std::cerr << "Failure allocating cpumask" << std::endl;
        exit(-1);
      }

      if (numa_node_to_cpus(i, &*nodecpus[i])) {
        std::cerr << "Could not get the CPUs of the node" << std::endl;
        exit(-1);
      }
    }

    if (n_cores != static_cast<uint32_t>(get_nprocs())) {
      std::cerr << "numa_num_task_cpus=" << n_cores
                << " != nproc=" << get_nprocs() << std::endl;
      exit(-1);
    }

    std::vector<core_id> core_ids{};
    thread_to_core_map.resize(n_cores);
    core_to_node_map.resize(512);

    uint32_t i = 0;
    switch (nm_policy) {
     case NUMA_FILL:
      std::cerr << "Using NUMA fill policy; ignoring hyperthread policy"
                << std::endl;
        for (uint32_t n = 0; n < n_nodes; n++) {
          for (uint32_t c = 0; c < n_cores / 2; c++) {
            if (numa_bitmask_isbitset(&*nodecpus[n], c))
              core_to_node_map[c] = n;
              thread_to_core_map[i++] = c;
          }
        }
        for (uint32_t n = 0; n < n_nodes; n++) {
          for (uint32_t c = n_cores / 2; c < n_cores; c++) {
            if (numa_bitmask_isbitset(&*nodecpus[n], c))
              core_to_node_map[c] = n;
              thread_to_core_map[i++] = c;
          }
        }
        break;

     case NUMA_INTERLEAVE:
      std::cerr << "Using NUMA interleave policy; ignoring hyperthread policy"
                << std::endl;
        for (uint32_t n = 0; n < n_nodes; n++) {
          i = 0;
          for (uint32_t c = 0; c < n_cores; c++) {
            if (numa_bitmask_isbitset(&*nodecpus[n], c))
              core_to_node_map[c] = n;
              // thread_to_node_map[n + (i++) * n_nodes] = n;
              thread_to_core_map[n + (i++) * n_nodes] = c;
          }
        }
        break;

     default:
      std::cerr << "Invalid NUMA policy" << std::endl;
      exit(-1);
    }
  }

  // Returns core_id of the core this thread was pinned to (which is controlled
  // by the topology and fill policy.
  uint32_t pin(uint32_t thread_id) {
    uint32_t core_id = thread_to_core_map[thread_id];
    std::cerr << "Pinning thread " << thread_id << " to core " << core_id << std::endl;
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0) {
      std::cerr << "setaffinity failed" << std::endl;
    }
    return core_id;
  }



  uint32_t n_active_nodes(uint32_t n_threads, uint32_t (*get_node_id)(uint32_t)) {
    std::unordered_set<uint32_t> active_nodes{};
    for (uint32_t i = 0; i < n_threads; ++i)
      active_nodes.insert(get_node_id(thread_to_core_map[i]));
    return active_nodes.size();
  }

  numa_policy get_numa_policy() { return nm_policy; }
  core_policy get_core_policy() { return cr_policy; }
};

#endif
