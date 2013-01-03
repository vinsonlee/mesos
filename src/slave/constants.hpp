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

#ifndef __SLAVE_CONSTANTS_HPP__
#define __SLAVE_CONSTANTS_HPP__

#include <stdint.h>

#include <stout/duration.hpp>

namespace mesos {
namespace internal {
namespace slave {

// TODO(bmahler): It appears there may be a bug with gcc-4.1.2 in which these
// duration constants were not being initialized when having static linkage.
// This issue did not manifest in newer gcc's. Specifically, 4.2.1 was ok.
// So we've moved these to have external linkage but perhaps in the future
// we can revert this.

extern const Duration EXECUTOR_SHUTDOWN_GRACE_PERIOD;
extern const Duration STATUS_UPDATE_RETRY_INTERVAL;
extern const Duration GC_DELAY;
extern const Duration DISK_WATCH_INTERVAL;

// Maximum number of completed frameworks to store in memory.
extern const uint32_t MAX_COMPLETED_FRAMEWORKS;

// Maximum number of completed executors per framework to store in memory.
extern const uint32_t MAX_COMPLETED_EXECUTORS_PER_FRAMEWORK;

// Maximum number of completed tasks per executor to store in memory.
extern const uint32_t MAX_COMPLETED_TASKS_PER_EXECUTOR;

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __SLAVE_CONSTANTS_HPP__
