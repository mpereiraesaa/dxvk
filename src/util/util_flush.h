#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxvk {

  /**
   * \brief GPU context flush type
   */
  enum class GpuFlushType : uint32_t {
    /** Flush or Present called by application */
    ExplicitFlush           = 0,
    /** Function that requires GPU synchronization and
     *  may require a flush called by application */
    ImplicitSynchronization = 1,
    /** GPU command that applications are likely to synchronize
     *  with soon has been recorded into the command list */
    ImplicitStrongHint      = 2,
    /** A render pass boundary is likely to occur and a flush
     *  should be recorded if the command list is large enough. */
    ImplicitMediumHint      = 3,
    /** GPU commands have been recorded and a flush should be
     *  performed if the current command list is large enough. */
    ImplicitWeakHint        = 4,
  };


  /**
   * \brief GPU flush tracker
   *
   * Helper class that implements a context flush
   * heuristic for various scenarios.
   */
  class GpuFlushTracker {

  public:

    /**
     * \brief Creates flush tracker
     *
     * \param [in] maxAllowed Strongest implicit flush type to honour
     * \param [in] chunkScale Multiplier applied to the chunk count
     *    thresholds of the implicit flush heuristic. 1.0 keeps the
     *    built-in thresholds, larger values make implicit flushes rarer.
     */
    GpuFlushTracker(GpuFlushType maxAllowed, float chunkScale);

    /**
     * \brief Checks whether a context flush should be performed
     *
     * Note that this modifies internal state, and depending on the
     * flush type, this may influence the decision for future flushes.
     * \param [in] flushType Flush type
     * \param [in] chunkId GPU command sequence number
     * \param [in] lastCompleteSubmissionId Last completed command submission ID
     * \returns \c true if a flush should be performed
     */
    bool considerFlush(
            GpuFlushType          flushType,
            uint64_t              chunkId,
            uint32_t              lastCompleteSubmissionId);

    /**
     * \brief Notifies tracker about a context flush
     *
     * \param [in] chunkId GPU command sequence number
     * \param [in] submissionId Command submission ID
     */
    void notifyFlush(
            uint64_t              chunkId,
            uint64_t              submissionId);

  private:

    GpuFlushType  m_maxType               = GpuFlushType::ImplicitWeakHint;
    GpuFlushType  m_lastMissedType        = GpuFlushType::ImplicitWeakHint;

    uint32_t      m_minChunkCount         =  3u;
    uint32_t      m_maxChunkCount         = 20u;

    uint64_t      m_lastFlushChunkId      = 0ull;
    uint64_t      m_lastFlushSubmissionId = 0ull;

  };

}
