//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.

#include "db/write_thread.h"

namespace rocksdb {

Status WriteThread::EnterWriteThread(WriteThread::Writer* w,
                                     uint64_t expiration_time) {
  // the following code block pushes the current writer "w" into the writer
  // queue "writers_" and wait until one of the following conditions met:
  // 1. the job of "w" has been done by some other writers.
  // 2. "w" becomes the first writer in "writers_"
  // 3. "w" timed-out.
  writers_.push_back(w);

  bool timed_out = false;
  while (!w->done && w->parallel_execute_id <= 0 && w != writers_.front()) {
    if (expiration_time == 0) {
      w->cv.Wait();
    } else if (w->cv.TimedWait(expiration_time)) {
      if (w->in_batch_group) {
        // then it means the front writer is currently doing the
        // write on behalf of this "timed-out" writer.  Then it
        // should wait until the write completes.
        expiration_time = 0;
      } else {
        timed_out = true;
        break;
      }
    }
  }

  if (!w->done && w->parallel_execute_id > 0) {
    return Status::OK();
  }

  if (timed_out) {
#ifndef NDEBUG
    bool found = false;
#endif
    for (auto iter = writers_.begin(); iter != writers_.end(); iter++) {
      if (*iter == w) {
        writers_.erase(iter);
#ifndef NDEBUG
        found = true;
#endif
        break;
      }
    }
#ifndef NDEBUG
    assert(found);
#endif
    // writers_.front() might still be in cond_wait without a time-out.
    // As a result, we need to signal it to wake it up.  Otherwise no
    // one else will wake him up, and RocksDB will hang.
    if (!writers_.empty()) {
      writers_.front()->cv.Signal();
    }
    return Status::TimedOut();
  }
  return Status::OK();
}

void WriteThread::StartParallelRun(WriteThread::Writer* w, uint32_t num_threads,
                                   WriteThread::Writer* last_writer) {
  assert(unfinished_threads_.load() == 0);
  unfinished_threads_.store(num_threads);
  int parallel_id = 1;
  while (!writers_.empty()) {
    Writer* parallel_writer = writers_.front();
    parallel_writers_.push_back(parallel_writer);
    parallel_writer->parallel_execute_id = parallel_id;
    parallel_id += parallel_writer->batch->Count();
    if (parallel_writer != w) {
      parallel_writer->cv.Signal();
    }
    if (parallel_writer != last_writer) {
      writers_.pop_front();
    } else {
      // Leave the last parallel writer so that the next one in queue
      // will not execute
      break;
    }
  }
  assert(num_threads == parallel_writers_.size());
}

bool WriteThread::ReportParallelRunFinish() {
  return unfinished_threads_.fetch_add(-1) == 1;
}

void WriteThread::LeaderWaitEndParallel(WriteThread::Writer* self) {
  while (unfinished_threads_.load() != 0) {
    self->cv.Wait();
  }
}

void WriteThread::LeaderEndParallel(WriteThread::Writer* self,
                                    WriteThread::Writer* last_writer,
                                    FlushScheduler* flush_scheduler) {
  assert(unfinished_threads_.load() == 0);
  // Tag all as done
  for (Writer* parallel_writer : parallel_writers_) {
    if (parallel_writer != self) {
      self->cfd_set.insert(parallel_writer->cfd_set.cbegin(),
                           parallel_writer->cfd_set.cend());

      InstrumentedMutexLock l(&parallel_writer->self_mutex);
      parallel_writer->done = true;
      parallel_writer->self_cv.Signal();
    }
  }
  assert(!writers_.empty());
  assert(parallel_writers_.back() == writers_.front());
  assert(parallel_writers_.back() == last_writer);

  for (auto* cfd : self->cfd_set) {
    if (cfd->mem()->ShouldScheduleFlush()) {
      flush_scheduler->ScheduleFlush(cfd);
      cfd->mem()->MarkFlushScheduled();
    }
  }

  parallel_writers_.clear();
  // Now the last parallel writer still in the writer queue
  // though it can be a dumb pointer.
  assert(!writers_.empty());
  assert(writers_.front() == last_writer);
  writers_.pop_front();
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }
}

void WriteThread::EndParallelRun(WriteThread::Writer* w,
                                 bool need_wake_up_leader,
                                 InstrumentedMutex* db_mutex) {
  if (need_wake_up_leader) {
    InstrumentedMutexLock l(db_mutex);
    // There is a race condition that leader already wakes up and
    // exit.
    if (!parallel_writers_.empty()) {
      Writer* leader = parallel_writers_.front();
      assert(leader != nullptr);
      // It can be signal to a wrong process but if that happened,
      // the leader already exits so we are fine.
      leader->cv.Signal();
    }
  }
  {
    InstrumentedMutexLock l(&w->self_mutex);
    while (!w->done) {
      w->self_cv.Wait();
    }
  }
}

void WriteThread::ExitWriteThread(WriteThread::Writer* w,
                                  WriteThread::Writer* last_writer,
                                  Status status) {
  // Pop out the current writer and all writers being pushed before the
  // current writer from the writer queue.
  while (!writers_.empty()) {
    Writer* ready = writers_.front();
    writers_.pop_front();
    if (ready != w) {
      ready->status = status;
      ready->done = true;
      ready->cv.Signal();
    }
    if (ready == last_writer) break;
  }

  // Notify new head of write queue
  if (!writers_.empty()) {
    writers_.front()->cv.Signal();
  }
}

// This function will be called only when the first writer succeeds.
// All writers in the to-be-built batch group will be processed.
//
// REQUIRES: Writer list must be non-empty
// REQUIRES: First writer must have a non-nullptr batch
size_t WriteThread::BuildBatchGroup(
    WriteThread::Writer** last_writer,
    autovector<WriteBatch*>* write_batch_group) {
  assert(!writers_.empty());
  Writer* first = writers_.front();
  assert(first->batch != nullptr);

  size_t size = WriteBatchInternal::ByteSize(first->batch);
  write_batch_group->push_back(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  size_t max_size = 1 << 20;
  if (size <= (128<<10)) {
    max_size = size + (128<<10);
  }

  *last_writer = first;

  if (first->has_callback) {
    // TODO(agiardullo:) Batching not currently supported as this write may
    // fail if the callback function decides to abort this write.
    return size;
  }

  std::deque<Writer*>::iterator iter = writers_.begin();
  ++iter;  // Advance past "first"
  for (; iter != writers_.end(); ++iter) {
    Writer* w = *iter;
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }

    if (!w->disableWAL && first->disableWAL) {
      // Do not include a write that needs WAL into a batch that has
      // WAL disabled.
      break;
    }

    if (w->timeout_hint_us < first->timeout_hint_us) {
      // Do not include those writes with shorter timeout.  Otherwise, we might
      // execute a write that should instead be aborted because of timeout.
      break;
    }

    if (w->has_callback) {
      // Do not include writes which may be aborted if the callback does not
      // succeed.
      break;
    }

    if (w->batch == nullptr) {
      // Do not include those writes with nullptr batch. Those are not writes,
      // those are something else. They want to be alone
      break;
    }

    size += WriteBatchInternal::ByteSize(w->batch);
    if (size > max_size) {
      // Do not make batch too big
      break;
    }

    write_batch_group->push_back(w->batch);
    w->in_batch_group = true;
    *last_writer = w;
  }
  return size;
}

}  // namespace rocksdb
