#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <new>
#include <utility>
#include <vector>

#include "../util/log/log.h"
#include "../util/thread.h"
#include "../util/util_env.h"

/**
 * \brief Deferred D3D7 command execution (ddraw.deferredMode)
 *
 * Hot calls made by the application thread (state changes, draws, the windowed
 * present blit) are recorded into a queue and executed in order on a worker
 * thread, so the application can prepare the next frame while the previous
 * one is being translated to D3D9. Every other entry point calls
 * D7VK_DEFERRED_SYNC() first, which waits until all recorded calls have run,
 * so it observes exactly the state it would have seen without deferral.
 *
 * Single producer: only the thread that created the device records calls.
 * Calls made from the worker itself (the recorded lambdas re-enter the same
 * entry points) are never deferred.
 */
namespace dxvk::d7q {

  constexpr uint32_t QueueAlignment = 16u;

  constexpr uint32_t AlignQueueSize(size_t size) {
    return uint32_t((size + QueueAlignment - 1u) & ~size_t(QueueAlignment - 1u));
  }

  class Queue {

  public:

    Queue() {
      m_producerThread = dxvk::this_thread::get_id();
      m_current = AllocChunk(ChunkSize);
      m_chunks.push_back(m_current);
      m_thread = dxvk::thread([this] { Run(); });
    }

    Queue(const Queue&) = delete;
    Queue& operator = (const Queue&) = delete;

    bool IsProducerThread() const {
      return dxvk::this_thread::get_id() == m_producerThread;
    }

    /**
     * \brief Ensures the next \p size bytes of Copy/Push calls end up in the same chunk
     *
     * Data returned by Copy() lives in the chunk it was written to, so a command
     * that references copied data must be recorded right after the copies,
     * following a Reserve() call that covers both.
     */
    void Reserve(uint32_t size) {
      if (m_current->capacity - m_writeOffset < size)
        NextChunk(size);
    }

    /**
     * \brief Copies data into the queue
     * \returns Pointer that stays valid until the command recorded next has executed
     */
    void* Copy(const void* data, size_t size) {
      void* dst = AllocRecord(size, true);
      std::memcpy(dst, data, size);
      return dst;
    }

    /**
     * \brief Records a call to run on the worker thread
     */
    template<typename Fn>
    void Push(Fn&& fn) {
      using CmdType = Cmd<std::decay_t<Fn>>;
      void* mem = AllocRecord(sizeof(CmdType), false);
      new (mem) CmdType(std::forward<Fn>(fn));

      m_pushedCmds += 1;

      if (!(m_pushedCmds % SubmitInterval))
        Submit();
    }

    /**
     * \brief Makes all recorded commands visible to the worker and wakes it if needed
     */
    void Submit() {
      // seq_cst: pairs with the worker publishing m_workerWaiting before it
      // re-checks for work, so one side always sees the other's write
      m_current->committed.store(m_writeOffset);
      m_submittedCmds.store(m_pushedCmds);

      if (m_workerWaiting.load()) {
        std::lock_guard<dxvk::mutex> lock(m_mutex);
        m_workCond.notify_one();
      }
    }

    /**
     * \brief Waits until every recorded command has executed
     */
    void Sync() {
      uint64_t target;

      if (IsProducerThread()) {
        target = m_pushedCmds;

        if (m_executedCmds.load(std::memory_order_acquire) == target)
          return;

        Submit();
      } else {
        // Other threads must not touch producer state; D3D7 calls from several
        // threads at once are only safe with DDSCL_MULTITHREADED device locking
        target = m_submittedCmds.load();
      }

      std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_idleCond.wait(lock, [this, target] {
        return m_executedCmds.load(std::memory_order_acquire) >= target;
      });
    }

    /**
     * \brief Called by the application after recording a present
     *
     * Blocks while the worker is more than one frame behind.
     */
    void FramePresented() {
      m_presentedFrames += 1;
      Submit();

      if (m_presentedFrames - m_executedFrames.load(std::memory_order_acquire) <= MaxFramesAhead)
        return;

      std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_idleCond.wait(lock, [this] {
        return m_presentedFrames - m_executedFrames.load(std::memory_order_acquire) <= MaxFramesAhead;
      });
    }

    /**
     * \brief Called by the worker after executing a recorded present
     */
    void FrameExecuted() {
      m_executedFrames.fetch_add(1, std::memory_order_release);
      // Wake the application now rather than after the rest of the batch
      NotifyIdle();
    }

  private:

    static constexpr uint32_t ChunkSize      = 1u << 20;
    static constexpr uint32_t SubmitInterval = 64u;
    static constexpr uint64_t MaxFramesAhead = 1u;

    struct CmdBase {
      virtual ~CmdBase() { }
      virtual void Exec() = 0;
    };

    template<typename Fn>
    struct Cmd : CmdBase {
      Fn fn;

      explicit Cmd(Fn&& f)
      : fn(std::move(f)) { }

      void Exec() override { fn(); }
    };

    // A chunk holds a sequence of records, each a command or a block of copied data
    struct Chunk {
      uint32_t              capacity  = 0;
      std::atomic<uint32_t> committed = { 0u };  // bytes visible to the worker
      std::atomic<bool>     sealed    = { false }; // producer moved on to a newer chunk
      uint8_t*              data      = nullptr;
    };

    // Records in a chunk are either commands or raw data blocks. Each starts
    // with a tag so the worker can step over data it doesn't execute.
    struct RecordHeader {
      uint32_t size;    // total record size including this header, aligned
      uint32_t isData;
    };

    static constexpr uint32_t HeaderSize = AlignQueueSize(sizeof(RecordHeader));

    static Chunk* AllocChunk(uint32_t capacity) {
      Chunk* chunk = new Chunk();
      chunk->capacity = capacity;
      chunk->data = static_cast<uint8_t*>(::operator new(capacity, std::align_val_t(QueueAlignment)));
      return chunk;
    }

    static void FreeChunk(Chunk* chunk) {
      ::operator delete(chunk->data, std::align_val_t(QueueAlignment));
      delete chunk;
    }

    // Reserves one record in the current chunk and returns its payload
    void* AllocRecord(size_t payloadSize, bool isData) {
      const uint32_t recordSize = HeaderSize + AlignQueueSize(payloadSize);

      if (m_current->capacity - m_writeOffset < recordSize)
        NextChunk(recordSize);

      uint8_t* base = m_current->data + m_writeOffset;
      auto* header = reinterpret_cast<RecordHeader*>(base);
      header->size   = recordSize;
      header->isData = isData ? 1u : 0u;

      m_writeOffset += recordSize;
      return base + HeaderSize;
    }

    void NextChunk(uint32_t minSize) {
      Chunk* next = nullptr;

      {
        std::lock_guard<dxvk::mutex> lock(m_mutex);

        // Reuse a retired standard-size chunk rather than allocating fresh pages
        if (minSize <= ChunkSize && !m_freeChunks.empty()) {
          next = m_freeChunks.back();
          m_freeChunks.pop_back();
          next->committed.store(0u);
          next->sealed.store(false);
        }
      }

      if (next == nullptr)
        next = AllocChunk(std::max(ChunkSize, minSize));

      {
        std::lock_guard<dxvk::mutex> lock(m_mutex);
        m_chunks.push_back(next);
      }

      // Publish the final size of the old chunk before sealing it
      m_current->committed.store(m_writeOffset);
      m_current->sealed.store(true);

      m_current     = next;
      m_writeOffset = 0;
    }

    void Run() {
      env::setThreadName("d7vk-deferred");
      t_isWorker = true;

      Chunk*   chunk     = nullptr;
      Chunk*   retired   = nullptr;
      uint32_t readOffset = 0;

      while (true) {
        if (chunk == nullptr) {
          std::lock_guard<dxvk::mutex> lock(m_mutex);
          chunk = m_chunks.front();
        }

        const uint32_t committed = chunk->committed.load(std::memory_order_acquire);

        if (readOffset < committed) {
          while (readOffset < committed) {
            uint8_t* base = chunk->data + readOffset;
            auto* header = reinterpret_cast<RecordHeader*>(base);
            readOffset += header->size;

            if (header->isData)
              continue;

            auto* cmd = reinterpret_cast<CmdBase*>(base + HeaderSize);
            try {
              cmd->Exec();
            } catch (const DxvkError& e) {
              Logger::err(e.message());
            }
            cmd->~CmdBase();

            m_executedCmds.fetch_add(1, std::memory_order_release);
          }

          NotifyIdle();
          continue;
        }

        if (chunk->sealed.load(std::memory_order_acquire)
         && readOffset == chunk->committed.load(std::memory_order_acquire)) {
          // Keep one finished chunk around: a command at the start of this chunk's
          // successor may still reference data copied at the end of this one.
          std::lock_guard<dxvk::mutex> lock(m_mutex);
          m_chunks.pop_front();
          if (retired != nullptr) {
            if (retired->capacity == ChunkSize)
              m_freeChunks.push_back(retired);
            else
              FreeChunk(retired);
          }
          retired    = chunk;
          chunk      = nullptr;
          readOffset = 0;
          continue;
        }

        // Nothing to do: sleep until the producer submits more work
        std::unique_lock<dxvk::mutex> lock(m_mutex);
        m_workerWaiting.store(true);
        m_workCond.wait(lock, [&] {
          return chunk->committed.load() > readOffset
              || chunk->sealed.load();
        });
        m_workerWaiting.store(false);
      }
    }

    void NotifyIdle() {
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      m_idleCond.notify_all();
    }

    // Producer-only state
    Chunk*   m_current         = nullptr;
    uint32_t m_writeOffset     = 0;
    uint64_t m_pushedCmds      = 0;
    uint64_t m_presentedFrames = 0;

    decltype(dxvk::this_thread::get_id()) m_producerThread = { };

    // Shared state
    std::atomic<uint64_t> m_submittedCmds  = { 0u };
    std::atomic<uint64_t> m_executedCmds   = { 0u };
    std::atomic<uint64_t> m_executedFrames = { 0u };
    std::atomic<bool>     m_workerWaiting  = { false };

    dxvk::mutex              m_mutex;
    dxvk::condition_variable m_workCond;
    dxvk::condition_variable m_idleCond;
    std::deque<Chunk*>       m_chunks;
    std::vector<Chunk*>      m_freeChunks;

    dxvk::thread m_thread;

    static inline thread_local bool t_isWorker = false;

    friend bool IsWorkerThread();
  };

  inline Queue* g_queue = nullptr;

  inline bool IsWorkerThread() {
    return Queue::t_isWorker;
  }

  /**
   * \brief Starts deferred execution; called once a device exists
   */
  inline void Start() {
    if (g_queue != nullptr)
      return;

    g_queue = new Queue();
    Logger::info("D7VK: Deferred mode enabled");
  }

  /**
   * \brief Whether the current call should be recorded instead of executed
   */
  inline bool Defer() {
    return g_queue != nullptr && !IsWorkerThread() && g_queue->IsProducerThread();
  }

  /**
   * \brief Waits for all recorded calls; no-op without deferral or on the worker
   */
  inline void Sync() {
    if (g_queue != nullptr && !IsWorkerThread())
      g_queue->Sync();
  }

}

#define D7VK_DEFERRED_SYNC() ::dxvk::d7q::Sync()
