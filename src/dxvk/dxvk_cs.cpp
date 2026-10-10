#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <vector>
#include <x86intrin.h>

#include "dxvk_cs.h"

#include "../util/util_env.h"

namespace dxvk {

  bool DxvkCsProfile::s_enabled = false;

  namespace {

    struct DxvkCsProfileEntry {
      uint32_t id    = 0;
      uint64_t calls = 0;
      uint64_t ticks = 0;
    };

    struct DxvkCsProfileState {
      bool     onCsThread = false;
      uint32_t nextId     = 1;
      uint64_t lastFrame  = 0;
      uint64_t lastTsc    = 0;
      uint64_t busy       = 0;
      uint64_t chunks     = 0;
      uint64_t commands   = 0;
      std::unordered_map<const char*, DxvkCsProfileEntry> entries;
    };

    // The CS thread owns the state; no other thread touches it.
    thread_local DxvkCsProfileState g_csProfile;

    uint64_t measureTscHz() {
      LARGE_INTEGER frequency = { };
      LARGE_INTEGER start = { };
      LARGE_INTEGER now = { };

      if (!QueryPerformanceFrequency(&frequency) || !frequency.QuadPart
       || !QueryPerformanceCounter(&start))
        return 0;

      uint64_t tsc0 = __rdtsc();

      do {
        if (!QueryPerformanceCounter(&now))
          return 0;
      } while (uint64_t(now.QuadPart - start.QuadPart) * 50 < uint64_t(frequency.QuadPart));

      uint64_t tsc1 = __rdtsc();
      uint64_t elapsed = uint64_t(now.QuadPart - start.QuadPart);
      return elapsed ? (tsc1 - tsc0) * uint64_t(frequency.QuadPart) / elapsed : 0;
    }

  }


  void DxvkCsProfile::start() {
    auto& state = g_csProfile;
    state.onCsThread = true;
    state.lastTsc = __rdtsc();

    if (env::getEnvVar("DXVK_CS_PROFILE") != "1")
      return;

    s_enabled = true;
    std::fprintf(stderr, "DXVK_CS_PROFILE version=1 start tsc_hz=%llu\n",
      static_cast<unsigned long long>(measureTscHz()));
  }


  void DxvkCsProfile::record(const char* name, uint64_t ticks) {
    auto& state = g_csProfile;

    if (!state.onCsThread)
      return;

    auto& entry = state.entries[name];

    if (!entry.id) {
      entry.id = state.nextId++;
      std::fprintf(stderr, "DXVK_CS_PROFILE name id=%u text=%s\n", entry.id, name);
    }

    entry.calls += 1;
    entry.ticks += ticks;
    state.busy += ticks;
    state.commands += 1;
  }


  void DxvkCsProfile::chunk() {
    if (g_csProfile.onCsThread)
      g_csProfile.chunks += 1;
  }


  void DxvkCsProfile::endFrame(uint64_t frameId) {
    auto& state = g_csProfile;

    if (!state.onCsThread)
      return;

    uint64_t tsc = __rdtsc();

    std::vector<const DxvkCsProfileEntry*> sorted;
    sorted.reserve(state.entries.size());

    for (const auto& pair : state.entries) {
      if (pair.second.calls)
        sorted.push_back(&pair.second);
    }

    std::sort(sorted.begin(), sorted.end(), [] (const DxvkCsProfileEntry* a, const DxvkCsProfileEntry* b) {
      return a->ticks != b->ticks ? a->ticks > b->ticks : a->id < b->id;
    });

    // One ps5log record holds 1024 bytes; twelve entries fit with room.
    char line[1000];
    int used = std::snprintf(line, sizeof(line),
      "DXVK_CS_PROFILE version=1 frame=%llu frames=%llu tsc=%llu wall=%llu busy=%llu chunks=%llu cmds=%llu",
      static_cast<unsigned long long>(frameId),
      static_cast<unsigned long long>(frameId > state.lastFrame ? frameId - state.lastFrame : 0),
      static_cast<unsigned long long>(tsc),
      static_cast<unsigned long long>(tsc > state.lastTsc ? tsc - state.lastTsc : 0),
      static_cast<unsigned long long>(state.busy),
      static_cast<unsigned long long>(state.chunks),
      static_cast<unsigned long long>(state.commands));

    uint64_t otherCalls = 0;
    uint64_t otherTicks = 0;

    for (size_t i = 0; i < sorted.size(); i++) {
      if (i < 12 && used > 0 && size_t(used) < sizeof(line)) {
        int n = std::snprintf(line + used, sizeof(line) - used, " c%u=%llu/%llu", sorted[i]->id,
          static_cast<unsigned long long>(sorted[i]->calls),
          static_cast<unsigned long long>(sorted[i]->ticks));

        if (n > 0 && size_t(used + n) < sizeof(line)) {
          used += n;
          continue;
        }

        line[used] = '\0';
      }

      otherCalls += sorted[i]->calls;
      otherTicks += sorted[i]->ticks;
    }

    if (otherCalls && used > 0 && size_t(used) < sizeof(line)) {
      int n = std::snprintf(line + used, sizeof(line) - used, " other=%llu/%llu",
        static_cast<unsigned long long>(otherCalls),
        static_cast<unsigned long long>(otherTicks));

      if (n <= 0 || size_t(used + n) >= sizeof(line))
        line[used] = '\0';
    }

    if (used > 0)
      std::fprintf(stderr, "%s\n", line);

    for (auto& pair : state.entries) {
      pair.second.calls = 0;
      pair.second.ticks = 0;
    }

    state.lastFrame = frameId;
    state.lastTsc = tsc;
    state.busy = 0;
    state.chunks = 0;
    state.commands = 0;
  }

  
  DxvkCsChunk::DxvkCsChunk() {
    
  }
  
  
  DxvkCsChunk::~DxvkCsChunk() {
    this->reset();
  }
  
  
  void DxvkCsChunk::init(DxvkCsChunkFlags flags) {
    m_flags = flags;
  }


  void DxvkCsChunk::executeAll(DxvkContext* ctx) {
    auto cmd = m_head;

    if (unlikely(DxvkCsProfile::enabled())) {
      DxvkCsProfile::chunk();

      bool singleUse = m_flags.test(DxvkCsChunkFlag::SingleUse);

      if (singleUse)
        m_commandOffset = 0;

      while (cmd != nullptr) {
        auto next = cmd->next();
        uint64_t t0 = __rdtsc();
        cmd->exec(ctx);
        uint64_t t1 = __rdtsc();
        DxvkCsProfile::record(cmd->profileName(), t1 - t0);

        if (singleUse)
          cmd->~DxvkCsCmd();

        cmd = next;
      }

      if (singleUse) {
        m_head = nullptr;
        m_next = &m_head;
      }

      return;
    }
    
    if (m_flags.test(DxvkCsChunkFlag::SingleUse)) {
      m_commandOffset = 0;
      
      while (cmd != nullptr) {
        auto next = cmd->next();
        cmd->exec(ctx);
        cmd->~DxvkCsCmd();
        cmd = next;
      }

      m_head = nullptr;
      m_next = &m_head;
    } else {
      while (cmd != nullptr) {
        cmd->exec(ctx);
        cmd = cmd->next();
      }
    }
  }
  
  
  void DxvkCsChunk::reset() {
    auto cmd = m_head;

    while (cmd != nullptr) {
      auto next = cmd->next();
      cmd->~DxvkCsCmd();
      cmd = next;
    }
    
    m_head = nullptr;
    m_next = &m_head;

    m_commandOffset = 0;
  }
  
  
  DxvkCsChunkPool::DxvkCsChunkPool() {
    
  }
  
  
  DxvkCsChunkPool::~DxvkCsChunkPool() {
    for (DxvkCsChunk* chunk : m_chunks)
      delete chunk;
  }
  
  
  DxvkCsChunk* DxvkCsChunkPool::allocChunk(DxvkCsChunkFlags flags) {
    DxvkCsChunk* chunk = nullptr;

    { std::lock_guard<dxvk::mutex> lock(m_mutex);
      
      if (m_chunks.size() != 0) {
        chunk = m_chunks.back();
        m_chunks.pop_back();
      }
    }
    
    if (!chunk)
      chunk = new DxvkCsChunk();
    
    chunk->init(flags);
    return chunk;
  }
  
  
  void DxvkCsChunkPool::freeChunk(DxvkCsChunk* chunk) {
    chunk->reset();
    
    std::lock_guard<dxvk::mutex> lock(m_mutex);
    m_chunks.push_back(chunk);
  }
  
  
  DxvkCsThread::DxvkCsThread(
    const Rc<DxvkDevice>&   device,
    const Rc<DxvkContext>&  context)
  : m_device(device), m_context(context),
    m_thread([this] { threadFunc(); }) {
    
  }
  
  
  DxvkCsThread::~DxvkCsThread() {
    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }
    
    m_condOnAdd.notify_one();
    m_thread.join();
  }
  
  
  uint64_t DxvkCsThread::dispatchChunk(DxvkCsChunkRef&& chunk) {
    uint64_t seq;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      seq = ++m_queueOrdered.seqDispatch;

      auto& entry = m_queueOrdered.queue.emplace_back();
      entry.chunk = std::move(chunk);
      entry.seq = seq;
    }

    // Wake the CS thread after releasing the lock so
    // that it does not immediately block on it again
    m_condOnAdd.notify_one();
    
    return seq;
  }


  void DxvkCsThread::injectChunk(DxvkCsQueue queue, DxvkCsChunkRef&& chunk, bool synchronize) {
    uint64_t timeline = 0u;

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      auto& q = getQueue(queue);

      if (synchronize)
        timeline = ++q.seqDispatch;

      auto& entry = q.queue.emplace_back();
      entry.chunk = std::move(chunk);
      entry.seq = timeline;

      if (queue == DxvkCsQueue::HighPriority) {
        // Worker will check this flag after executing any
        // chunk without causing additional lock contention
        m_hasHighPrio.store(true, std::memory_order_release);
      }
    }

    m_condOnAdd.notify_one();

    if (synchronize) {
      std::unique_lock<dxvk::mutex> lock(m_counterMutex);

      m_condOnSync.wait(lock, [this, queue, timeline] {
        return getCounter(queue).load(std::memory_order_acquire) >= timeline;
      });
    }
  }


  void DxvkCsThread::synchronize(uint64_t seq) {
    // Avoid locking if we know the sync is a no-op, may
    // reduce overhead if this is being called frequently
    if (seq > m_seqOrdered.load(std::memory_order_acquire)) {
      // We don't need to lock the queue here, if synchronization
      // happens while another thread is submitting then there is
      // an inherent race anyway
      if (seq == SynchronizeAll)
        seq = m_queueOrdered.seqDispatch;

      auto t0 = dxvk::high_resolution_clock::now();

      { std::unique_lock<dxvk::mutex> lock(m_counterMutex);
        m_condOnSync.wait(lock, [this, seq] {
          return m_seqOrdered.load(std::memory_order_acquire) >= seq;
        });
      }

      auto t1 = dxvk::high_resolution_clock::now();
      auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);

      m_device->addStatCtr(DxvkStatCounter::CsSyncCount, 1);
      m_device->addStatCtr(DxvkStatCounter::CsSyncTicks, ticks.count());
    }
  }
  
  
  void DxvkCsThread::threadFunc() {
    env::setThreadName("dxvk-cs");
    DxvkCsProfile::start();

    // Local chunk queues, we use two queues and swap between
    // them in order to potentially reduce lock contention.
    std::vector<DxvkCsQueuedChunk> ordered;
    std::vector<DxvkCsQueuedChunk> highPrio;

    try {
      while (!m_stopped.load()) {
        { std::unique_lock<dxvk::mutex> lock(m_mutex);

          auto pred = [this] { return
              !m_queueOrdered.queue.empty()
              || !m_queueHighPrio.queue.empty()
              || m_stopped.load();
          };

          if (unlikely(!pred())) {
            auto t0 = dxvk::high_resolution_clock::now();

            m_condOnAdd.wait(lock, [&] {
              return pred();
            });

            auto t1 = dxvk::high_resolution_clock::now();
            m_device->addStatCtr(DxvkStatCounter::CsIdleTicks, std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
          }

          std::swap(ordered, m_queueOrdered.queue);
          std::swap(highPrio, m_queueHighPrio.queue);

          m_hasHighPrio.store(false, std::memory_order_release);
        }

        size_t orderedIndex = 0u;
        size_t highPrioIndex = 0u;

        while (highPrioIndex < highPrio.size() || orderedIndex < ordered.size()) {
          // Re-fill local high-priority queue if the app has queued anything up
          // in the meantime, we want to reduce possible synchronization delays.
          if (highPrioIndex >= highPrio.size() && m_hasHighPrio.load(std::memory_order_acquire)) {
            highPrio.clear();
            highPrioIndex = 0u;

            std::unique_lock<dxvk::mutex> lock(m_mutex);
            std::swap(highPrio, m_queueHighPrio.queue);

            m_hasHighPrio.store(false, std::memory_order_release);
          }

          // Drain high-priority queue first
          bool isHighPrio = highPrioIndex < highPrio.size();
          auto& entry = isHighPrio ? highPrio[highPrioIndex++] : ordered[orderedIndex++];

          m_context->addStatCtr(DxvkStatCounter::CsChunkCount, 1);

          entry.chunk->executeAll(m_context.ptr());

          if (entry.seq) {
            // Use a separate mutex for the chunk counter, this will only
            // ever be contested if synchronization is actually necessary.
            std::lock_guard lock(m_counterMutex);

            auto& counter = isHighPrio ? m_seqHighPrio : m_seqOrdered;
            counter.store(entry.seq, std::memory_order_release);

            m_condOnSync.notify_one();
          }

          // Immediately free the chunk to release
          // references to any resources held by it
          entry.chunk = DxvkCsChunkRef();
        }

        ordered.clear();
        highPrio.clear();
      }
    } catch (const DxvkError& e) {
      Logger::err("Exception on CS thread!");
      Logger::err(e.message());
    }
  }
  
}
