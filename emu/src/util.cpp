/*
 * Copyright (c) 2020 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "util.h"
#include <mutex>

// generic mutex object to protect structures written to by multiple threads
std::mutex m;
// STL map of std thread ID to PE/core ID
std::map<std::thread::id, unsigned> tid_to_core_id_map;
// STL map of PE/core ID to pointer to queue intfc object
std::map<std::pair<unsigned, unsigned>, q_intfc_t *> queues;
// Vector of pointers to per-PE mutex and cv objects
std::vector<std::condition_variable *> cvs;
std::vector<std::mutex *> mutexes;
// Vector of sleep_cntr to implement sleep/wake
std::vector<int> sleep_cntr;
// record own user-specified PE/core ID
void __register_core_id(unsigned core_id) {
  m.lock();
    tid_to_core_id_map.insert(std::make_pair(std::this_thread::get_id(), core_id));
  m.unlock();
}
void __init(unsigned long num_pe, unsigned long wq_depth) {
    // create one cv and mutex object per PE
    for (unsigned i = 0; i < num_pe; ++i) {
        sleep_cntr.push_back(0);
        cvs.push_back(new std::condition_variable);
        mutexes.push_back(new std::mutex);
    }

    // init queues
    unsigned depth = wq_depth;
    // queues.at(std::make_pair(source, sink))(new q_intfc_t(depth));
    // begin generated code for init_queues
    queues.insert(std::make_pair(std::make_pair(0, 1), new q_intfc_t(depth)));
    queues.insert(std::make_pair(std::make_pair(0, 2), new q_intfc_t(depth)));
    queues.insert(std::make_pair(std::make_pair(0, 3), new q_intfc_t(depth)));
    queues.insert(std::make_pair(std::make_pair(0, 4), new q_intfc_t(depth)));
    // end generated code for init_queues
}
void __teardown(void) {
    // teardown per PE cvs and mutexes
    for (auto &e : cvs) { delete e; }
    cvs.clear();
    for (auto &e : mutexes) { delete e; }
    mutexes.clear();

    // teardown queues
    for (auto &e : queues) { delete e.second; }
    queues.clear();

    // teardown tid_to_core_id_map
    tid_to_core_id_map.clear();
}
// work queue interface
void __push(unsigned sink, uint64_t data) {
    __push_mmap(sink, data);
}
void __push_mmap(unsigned sink, uint64_t data) {
    unsigned source;
    q_intfc_t *queue;
    source = get_id();
    try {
        queue = queues.at(std::make_pair(source, sink));
    } catch (const std::out_of_range &e) {
        printf("%s(): exception occured \"%s\" for source=%u, sink=%u, aborting. Perhaps you "
               "didn't run scripts/populate_init_queues.py?\n",
               __FUNCTION__, e.what(), source, sink);
        exit(1);
    }
    std::queue<uint64_t> &q = queue->q;
    std::mutex *m = queue->m;
    std::condition_variable *emptyCv = queue->emptyCv;
    std::condition_variable *fullCv = queue->fullCv;

    // grab the lock to this queue
    std::unique_lock<std::mutex> lock(*m);
    while (q.size() == queue->size) {
        // release lock and sleep until consumer PE notifies
        fullCv->wait(lock);
    }
    q.push(data);
    // if some PE is waiting on this queue, notify them
    emptyCv->notify_one();
}
uint64_t __pop(unsigned source) {
    return __pop_mmap(source);
}
uint64_t __pop_mmap(unsigned source) {
    unsigned sink;
    q_intfc_t *queue;
    sink = get_id();
    try {
        queue = queues.at(std::make_pair(source, sink));
    } catch (const std::out_of_range &e) {
        printf("%s(): exception occured \"%s\" for source=%u, sink=%u, aborting. Perhaps you "
               "didn't run scripts/populate_init_queues.py?\n",
               __FUNCTION__, e.what(), source, sink);
        exit(1);
    }
    std::queue<uint64_t> &q = queue->q;
    std::mutex *m = queue->m;
    std::condition_variable *emptyCv = queue->emptyCv;
    std::condition_variable *fullCv = queue->fullCv;

    uint64_t data;
    // grab the lock to this queue
    std::unique_lock<std::mutex> lock(*m);
    while (q.size() == 0) {
        // release lock and sleep until producer PE notifies
        emptyCv->wait(lock);
    }
    data = q.front();
    q.pop();
    // if some PE is waiting on this queue, notify them
    fullCv->notify_one();
    return data;
}
// barrier synchronization interface
void __barrier_init(pthread_barrier_t *bar, unsigned count) {
    pthread_barrier_init(bar, NULL, count);
}
void __barrier_wait(pthread_barrier_t *bar) { pthread_barrier_wait(bar); }

// mutex interface
void __mutex_init(pthread_mutex_t *lock) { pthread_mutex_init(lock, NULL); }
void __mutex_lock(pthread_mutex_t *lock) { pthread_mutex_lock(lock); }
void __mutex_unlock(pthread_mutex_t *lock) { pthread_mutex_unlock(lock); }

// sleep-wake
void __sleep() {
    // get self PE-ID
    unsigned pe_id = get_id();

    // increment sleep_cntr for self and wait on CV
    // if it is <= 0 it means that someone has already called wake on it
    std::mutex *m = mutexes.at(pe_id);
    std::unique_lock<std::mutex> lock(*m);
    ++sleep_cntr.at(pe_id);
    while(sleep_cntr.at(pe_id) > 0) {
        cvs.at(pe_id)->wait(lock);
    }
}
void __wake(unsigned pe_id) {
    // set sleep_cntr to false for target PE and notify
    std::unique_lock<std::mutex> lock(*mutexes.at(pe_id));
    --sleep_cntr.at(pe_id);
    cvs.at(pe_id)->notify_one();
}

// m5 op wrapper primitives
void __reset_stats() {}
void __dump_stats() {}
void __dump_reset_stats() {}
void __print_debug(unsigned code) {}

int get_id() {
  m.lock();

  int id;

  try {
      id = tid_to_core_id_map.at(std::this_thread::get_id());
  } catch (const std::out_of_range &e) {
      printf("%s(): exception occured \"%s\", aborting. Perhaps your threads didn't call "
	         "register_core_id()?\n",
	             __FUNCTION__, e.what());
      exit(1);
  }

  m.unlock();
  return id;
}
