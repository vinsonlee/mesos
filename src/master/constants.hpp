/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __MASTER_CONSTANTS_HPP__
#define __MASTER_CONSTANTS_HPP__

#include <stdint.h>

#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace master {

// TODO(benh): Add units after constants.
// TODO(benh): Also make configuration options be constants.

// TODO(bmahler): It appears there may be a bug with gcc-4.1.2 in which the
// duration constants were not being initialized when having static linkage.
// This issue did not manifest in newer gcc's. Specifically, 4.2.1 was ok.
// So we've moved these to have external linkage but perhaps in the future
// we can revert this.


// TODO(tdmackey):NET? more explicit?

// Maximum number of slot offers to have outstanding for each framework.
extern const int MAX_OFFERS_PER_FRAMEWORK;

// Minimum number of cpus / task.
extern const uint32_t MIN_CPUS;

// Minimum amount of network bandwidth / task.
extern const uint32_t MIN_NET;

// Minimum amount of memory / task.
extern const uint32_t MIN_MEM;

// Maximum number of CPUs per machine.
extern const uint32_t MAX_CPUS;

// Maximum amount of network bandwidth / machine.
extern const uint32_t MAX_NET;

// Maximum amount of memory / machine.
extern const uint32_t MAX_MEM;

// Amount of time within which a slave PING should be received.
extern const Duration SLAVE_PING_TIMEOUT;

// Maximum number of ping timeouts until slave is considered failed.
extern const uint32_t MAX_SLAVE_PING_TIMEOUTS;

// Maximum number of completed frameworks to store in the cache.
// TODO(thomasm): Make configurable.
extern const uint32_t MAX_COMPLETED_FRAMEWORKS;

// Maximum number of completed tasks per framework to store in the
// cache.  TODO(thomasm): Make configurable.
extern const uint32_t MAX_COMPLETED_TASKS_PER_FRAMEWORK;

// Time interval to check for updated watchers list.
extern const Duration WHITELIST_WATCH_INTERVAL;

} // namespace mesos {
} // namespace internal {
} // namespace master {

#endif // __MASTER_CONSTANTS_HPP__
