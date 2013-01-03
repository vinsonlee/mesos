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

#include "common/units.hpp"

#include "master/constants.hpp"

namespace mesos {
namespace internal {
namespace master {

// TODO(tdmackey): MIN/MAX network bandwidth

const int MAX_OFFERS_PER_FRAMEWORK = 50;
const uint32_t MIN_CPUS = 1;
const uint32_t MIN_NET = 1;
const uint32_t MIN_MEM = 32 * Megabyte;
const uint32_t MAX_CPUS = 1000 * 1000;
const uint32_t MAX_NET = 100 *1024 * Megabit;
const uint32_t MAX_MEM = 1024 * 1024 * Megabyte;
const Duration SLAVE_PING_TIMEOUT = Seconds(15.0);
const uint32_t MAX_SLAVE_PING_TIMEOUTS = 5;
const uint32_t MAX_COMPLETED_FRAMEWORKS = 50;
const uint32_t MAX_COMPLETED_TASKS_PER_FRAMEWORK = 1000;
const Duration WHITELIST_WATCH_INTERVAL = Seconds(5.0);

} // namespace mesos {
} // namespace internal {
} // namespace master {
