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

#include <errno.h>
#include <fts.h>
#include <signal.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/types.h>

#include <glog/logging.h>

#include <fstream>
#include <list>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/io.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "linux/cgroups.hpp"
#include "linux/fs.hpp"
#include "linux/net.hpp"
#include "linux/proc.hpp"

using namespace process;

// TODO(benh): Move linux/fs.hpp out of 'mesos- namespace.
using namespace mesos::internal;

using std::list;
using std::map;
using std::set;
using std::string;
using std::vector;

namespace cgroups {
namespace internal {

// Snapshot of a subsystem (modeled after a line in /proc/cgroups).
struct SubsystemInfo
{
  SubsystemInfo()
    : hierarchy(0),
      cgroups(0),
      enabled(false) {}

  SubsystemInfo(const string& _name,
                int _hierarchy,
                int _cgroups,
                bool _enabled)
    : name(_name),
      hierarchy(_hierarchy),
      cgroups(_cgroups),
      enabled(_enabled) {}

  string name;      // Name of the subsystem.
  int hierarchy;    // ID of the hierarchy the subsystem is attached to.
  int cgroups;      // Number of cgroups for the subsystem.
  bool enabled;     // Whether the subsystem is enabled or not.
};


// Return information about subsystems on the current machine. We get
// information from /proc/cgroups file. Each line in it describes a subsystem.
// @return  A map from subsystem names to SubsystemInfo instances if succeeds.
//          Error if any unexpected happens.
static Try<map<string, SubsystemInfo> > subsystems()
{
  std::ifstream file("/proc/cgroups");

  if (!file.is_open()) {
    return Try<map<string, SubsystemInfo> >::error(
        "Failed to open /proc/cgroups");
  }

  map<string, SubsystemInfo> infos;

  while (!file.eof()) {
    string line;
    std::getline(file, line);

    if (file.fail()) {
      if (!file.eof()) {
        file.close();
        return Try<map<string, SubsystemInfo> >::error(
            "Failed to read /proc/cgroups");
      }
    } else {
      if (line.empty()) {
        // Skip empty lines.
        continue;
      } else if (line.find_first_of('#') == 0) {
        // Skip the first line which starts with '#' (contains titles).
        continue;
      } else {
        // Parse line to get subsystem info.
        string name;
        int hierarchy;
        int cgroups;
        bool enabled;

        std::istringstream ss(line);
        ss >> std::dec >> name >> hierarchy >> cgroups >> enabled;

        // Check for any read/parse errors.
        if (ss.fail() && !ss.eof()) {
          file.close();
          return Try<map<string, SubsystemInfo> >::error(
              "Failed to parse /proc/cgroups");
        }

        infos[name] = SubsystemInfo(name, hierarchy, cgroups, enabled);
      }
    }
  }

  file.close();
  return infos;
}


// Mount a cgroups virtual file system (with proper subsystems attached) to a
// given directory (hierarchy root). The cgroups virtual file system is the
// interface exposed by the kernel to control cgroups. Each directory created
// inside the hierarchy root is a cgroup. Therefore, cgroups are organized in a
// tree like structure. User can specify what subsystems to be attached to the
// hierarchy root so that these subsystems can be controlled through normal file
// system APIs. A subsystem can only be attached to one hierarchy. This function
// assumes the given hierarchy is an empty directory and the given subsystems
// are enabled in the current platform.
// @param   hierarchy   Path to the hierarchy root.
// @param   subsystems  Comma-separated subsystem names.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> mount(const string& hierarchy, const string& subsystems)
{
  return fs::mount(subsystems, hierarchy, "cgroup", 0, subsystems.c_str());
}


// Unmount the cgroups virtual file system from the given hierarchy root. Make
// sure to remove all cgroups in the hierarchy before unmount. This function
// assumes the given hierarchy is currently mounted with a cgroups virtual file
// system.
// @param   hierarchy   Path to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> unmount(const string& hierarchy)
{
  return fs::unmount(hierarchy);
}


// Copies the value of 'cpuset.cpus' and 'cpuset.mems' from a parent
// cgroup to a child cgroup so the child cgroup can actually run tasks
// (otherwise it gets the error 'Device or resource busy').
// @param   hierarchy      Path to hierarchy root.
// @param   parentCgroup   Path to parent cgroup relative to the hierarchy root.
// @param   childCgroup    Path to child cgroup relative to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> cloneCpusetCpusMems(
    const string& hierarchy,
    const string& parentCgroup,
    const string& childCgroup)
{
  Try<string> cpus = cgroups::read(hierarchy, parentCgroup, "cpuset.cpus");
  if (cpus.isError()) {
    return Try<Nothing>::error(
        "Failed to read control 'cpuset.cpus': " + cpus.error());
  }

  Try<string> mems = cgroups::read(hierarchy, parentCgroup, "cpuset.mems");
  if (mems.isError()) {
    return Try<Nothing>::error(
        "Failed to read control 'cpuset.mems': " + mems.error());
  }

  Try<Nothing> write = cgroups::write(
      hierarchy, childCgroup, "cpuset.cpus", cpus.get());
  if (write.isError()) {
    return Try<Nothing>::error(
        "Failed to write control 'cpuset.cpus': " + write.error());
  }

  write = cgroups::write(
      hierarchy, childCgroup, "cpuset.mems", mems.get());
  if (write.isError()) {
    return Try<Nothing>::error(
        "Failed to write control 'cpuset.mems': " + write.error());
  }

  return Nothing();
}


// Create a cgroup in a given hierarchy. To create a cgroup, one just need to
// create a directory in the cgroups virtual file system. The given cgroup is a
// relative path to the given hierarchy. This function assumes the given
// hierarchy is valid and is currently mounted with a cgroup virtual file
// system. The function also assumes the given cgroup is valid. This function
// will not create directories recursively, which means it will return error if
// any of the parent cgroups does not exist.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> create(const string& hierarchy, const string& cgroup)
{
  string path = path::join(hierarchy, cgroup);
  Try<Nothing> mkdir = os::mkdir(path, false); // Do NOT create recursively.
  if (mkdir.isError()) {
    return Try<Nothing>::error(
        "Failed to create directory at " + path + ": " + mkdir.error());
  }

  // Now clone 'cpuset.cpus' and 'cpuset.mems' if the 'cpuset'
  // subsystem is attached to the hierarchy.
  Try<set<string> > attached = cgroups::subsystems(hierarchy);
  if (attached.isError()) {
    return Try<Nothing>::error(
        "Failed to determine if hierarchy has the 'cpuset' "
        "subsystem attached: " + attached.error());
  } else if (attached.get().count("cpuset") > 0) {
    Try<string> parent = os::dirname(path::join("/", cgroup));
    if (parent.isError()) {
      return Try<Nothing>::error(
          "Failed to determine parent cgroup of " + cgroup +
          ": " + parent.error());
    }
    return cloneCpusetCpusMems(hierarchy, parent.get(), cgroup);
  }

  return Nothing();
}


// Remove a cgroup in a given hierarchy. To remove a cgroup, one needs to remove
// the corresponding directory in the cgroups virtual file system. A cgroup
// cannot be removed if it has processes or sub-cgroups inside. This function
// does nothing but tries to remove the corresponding directory of the given
// cgroup. It will return error if the remove operation fails because it has
// either processes or sub-cgroups inside.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> remove(const string& hierarchy, const string& cgroup)
{
  string path = path::join(hierarchy, cgroup);

  // Do NOT recursively remove cgroups.
  Try<Nothing> rmdir = os::rmdir(path, false);

  if (rmdir.isError()) {
    return Try<Nothing>::error(
        "Failed to remove cgroup at " + path + ": " + rmdir.error());
  }

  return rmdir;
}


// Read a control file. Control files are the gateway to monitor and control
// cgroups. This function assumes the cgroups virtual file systems are properly
// mounted on the given hierarchy, and the given cgroup has been already created
// properly. The given control file name should also be valid.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @return  The value read from the control file.
static Try<string> read(
    const string& hierarchy,
    const string& cgroup,
    const string& control)
{
  string path = path::join(hierarchy, cgroup, control);

  // We do not use os::read here because it cannot correctly read proc or
  // cgroups control files (lseek will return error).
  std::ifstream file(path.c_str());

  if (!file.is_open()) {
    return Try<string>::error("Failed to open file " + path);
  }

  std::ostringstream ss;
  ss << file.rdbuf();

  if (file.fail()) {
    // TODO(jieyu): Make sure that the way we get errno here is portable.
    string msg = strerror(errno);
    file.close();
    return Try<string>::error(msg);
  }

  file.close();
  return ss.str();
}


// Write a control file.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @param   value       Value to be written.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> write(
    const string& hierarchy,
    const string& cgroup,
    const string& control,
    const string& value)
{
  string path = path::join(hierarchy, cgroup, control);
  std::ofstream file(path.c_str());

  if (!file.is_open()) {
    return Try<Nothing>::error("Failed to open file " + path);
  }

  file << value << std::endl;

  if (file.fail()) {
    // TODO(jieyu): Make sure that the way we get errno here is portable.
    string msg = strerror(errno);
    file.close();
    return Try<Nothing>::error(msg);
  }

  file.close();
  return Nothing();
}

} // namespace internal {


// Returns some error string if either (a) hierarchy is not mounted,
// (b) cgroup does not exist, or (c) control file does not exist.
static Option<string> verify(
    const string& hierarchy,
    const string& cgroup = "",
    const string& control = "")
{
  Try<bool> mounted = cgroups::mounted(hierarchy);
  if (mounted.isError()) {
    return Option<string>::some(
        "Failed to determine if the hierarchy at " + hierarchy +
        " is mounted: " + mounted.error());
  } else if (!mounted.get()) {
    return Option<string>::some(hierarchy + " is not mounted");
  }

  if (cgroup != "") {
    if (!os::exists(path::join(hierarchy, cgroup))) {
      return Option<string>::some(cgroup + " does not exist");
    }
  }

  if (control != "") {
    CHECK(cgroup != "");
    if (!os::exists(path::join(hierarchy, cgroup, control))) {
      return Option<string>::some(
          "'" + control + "' does not exist (is subsystem attached?)");
    }
  }

  return Option<string>::none();
}


bool enabled()
{
  return os::exists("/proc/cgroups");
}


Try<set<string> > hierarchies()
{
  // Read currently mounted file systems from /proc/mounts.
  Try<fs::MountTable> table = fs::MountTable::read("/proc/mounts");
  if (table.isError()) {
    return Try<set<string> >::error(table.error());
  }

  set<string> results;
  foreach (const fs::MountTable::Entry& entry, table.get().entries) {
    if (entry.type == "cgroup") {
      Try<string> realpath = os::realpath(entry.dir);
      if (realpath.isError()) {
        return Try<set<string> >::error(
            "Failed to determine canonical path of " + entry.dir +
            ": " + realpath.error());
      }
      results.insert(realpath.get());
    }
  }

  return results;
}


Try<bool> enabled(const string& subsystems)
{
  Try<map<string, internal::SubsystemInfo> > infosResult =
    internal::subsystems();
  if (infosResult.isError()) {
    return Try<bool>::error(infosResult.error());
  }

  map<string, internal::SubsystemInfo> infos = infosResult.get();
  bool disabled = false;  // Whether some subsystems are not enabled.

  foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
    if (infos.find(subsystem) == infos.end()) {
      return Try<bool>::error("'" + subsystem + "' not found");
    }
    if (!infos[subsystem].enabled) {
      // Here, we don't return false immediately because we want to return
      // error if any of the given subsystems is missing.
      disabled = true;
    }
  }

  return !disabled;
}


Try<bool> busy(const string& subsystems)
{
  Try<map<string, internal::SubsystemInfo> > infosResult =
    internal::subsystems();
  if (infosResult.isError()) {
    return Try<bool>::error(infosResult.error());
  }

  map<string, internal::SubsystemInfo> infos = infosResult.get();
  bool busy = false;

  foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
    if (infos.find(subsystem) == infos.end()) {
      return Try<bool>::error("'" + subsystem + "' not found");
    }
    if (infos[subsystem].hierarchy != 0) {
      // Here, we don't return false immediately because we want to return
      // error if any of the given subsystems is missing.
      busy = true;
    }
  }

  return busy;
}


Try<set<string> > subsystems()
{
  Try<map<string, internal::SubsystemInfo> > infos = internal::subsystems();
  if (infos.isError()) {
    return Try<set<string> >::error(infos.error());
  }

  set<string> names;
  foreachvalue (const internal::SubsystemInfo& info, infos.get()) {
    if (info.enabled) {
      names.insert(info.name);
    }
  }

  return names;
}


Try<set<string> > subsystems(const string& hierarchy)
{
  // We compare the canonicalized absolute paths.
  Try<string> hierarchyAbsPath = os::realpath(hierarchy);
  if (hierarchyAbsPath.isError()) {
    return Try<set<string> >::error(
        "Failed to determine canonical path of " + hierarchy +
        ": " + hierarchyAbsPath.error());
  }

  // Read currently mounted file systems from /proc/mounts.
  Try<fs::MountTable> table = fs::MountTable::read("/proc/mounts");
  if (table.isError()) {
    return Try<set<string> >::error(
        "Failed to read mount table: " + table.error());
  }

  // Check if hierarchy is a mount point of type cgroup.
  Option<fs::MountTable::Entry> hierarchyEntry;
  foreach (const fs::MountTable::Entry& entry, table.get().entries) {
    if (entry.type == "cgroup") {
      Try<string> dirAbsPath = os::realpath(entry.dir);
      if (dirAbsPath.isError()) {
        return Try<set<string> >::error(
        "Failed to determine canonical path of " + entry.dir +
        ": " + dirAbsPath.error());
      }

      // Seems that a directory can be mounted more than once. Previous mounts
      // are obscured by the later mounts. Therefore, we must see all entries to
      // make sure we find the last one that matches.
      if (dirAbsPath.get() == hierarchyAbsPath.get()) {
        hierarchyEntry = entry;
      }
    }
  }

  if (hierarchyEntry.isNone()) {
    return Try<set<string> >::error(
        hierarchy + " is not a mount point for cgroups");
  }

  // Get the intersection of the currently enabled subsystems and mount
  // options. Notice that mount options may contain somethings (e.g. rw) that
  // are not in the set of enabled subsystems.
  Try<set<string> > names = subsystems();
  if (names.isError()) {
    return Try<set<string> >::error(names.error());
  }

  set<string> result;
  foreach (const string& name, names.get()) {
    if (hierarchyEntry.get().hasOption(name)) {
      result.insert(name);
    }
  }

  return result;
}


Try<Nothing> mount(const string& hierarchy, const string& subsystems)
{
  if (os::exists(hierarchy)) {
    return Try<Nothing>::error(
        hierarchy + " already exists in the file system");
  }

  // Make sure all subsystems are enabled and not busy.
  foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
    Try<bool> result = enabled(subsystem);
    if (result.isError()) {
      return Try<Nothing>::error(result.error());
    } else if (!result.get()) {
      return Try<Nothing>::error(
          "'" + subsystem + "' is not enabled by the kernel");
    }

    result = busy(subsystem);
    if (result.isError()) {
      return Try<Nothing>::error(result.error());
    } else if (result.get()) {
      return Try<Nothing>::error(
          "'" + subsystem + "' is already attached to another hierarchy");
    }
  }

  // Create the directory for the hierarchy.
  Try<Nothing> mkdir = os::mkdir(hierarchy);
  if (mkdir.isError()) {
    return Try<Nothing>::error(
        "Failed to mkdir " + hierarchy + ": " + mkdir.error());
  }

  // Mount the virtual file system (attach subsystems).
  Try<Nothing> result = internal::mount(hierarchy, subsystems);
  if (result.isError()) {
    // Do a best effort rmdir of hierarchy (ignoring success or failure).
    os::rmdir(hierarchy);
    return result;
  }

  return Nothing();
}


Try<Nothing> unmount(const string& hierarchy)
{
  Option<string> error = verify(hierarchy);
  if (error.isSome()) {
    return Try<Nothing>::error(error.get());
  }

  Try<Nothing> unmount = internal::unmount(hierarchy);
  if (unmount.isError()) {
    return unmount;
  }

  Try<Nothing> rmdir = os::rmdir(hierarchy);
  if (rmdir.isError()) {
    return Try<Nothing>::error(
        "Failed to remove directory at " + hierarchy + ": " + rmdir.error());
  }

  return Nothing();
}


Try<bool> mounted(const string& hierarchy, const string& subsystems)
{
  if (!os::exists(hierarchy)) {
    return false;
  }

  // We compare canonicalized absolute paths.
  Try<string> realpath = os::realpath(hierarchy);
  if (realpath.isError()) {
    return Try<bool>::error(
        "Failed to determine canonical path of " + hierarchy +
        ": " + realpath.error());
  }

  Try<set<string> > hierarchies = cgroups::hierarchies();
  if (hierarchies.isError()) {
    return Try<bool>::error(
        "Failed to get mounted hierarchies: " + hierarchies.error());
  }

  if (hierarchies.get().count(realpath.get()) == 0) {
    return false;
  }

  // Now make sure all the specified subsytems are attached.
  Try<set<string> > attached = cgroups::subsystems(hierarchy);
  if (attached.isError()) {
    return Try<bool>::error(
        "Failed to get subsystems attached to hierarchy " +
        hierarchy + ": " + attached.error());
  }

  foreach (const string& subsystem, strings::tokenize(subsystems, ",")) {
    if (attached.get().count(subsystem) == 0) {
      return false;
    }
  }

  return true;
}


Try<Nothing> create(const string& hierarchy, const string& cgroup)
{
  Option<string> error = verify(hierarchy);
  if (error.isSome()) {
    return Try<Nothing>::error(error.get());
  }

  return internal::create(hierarchy, cgroup);
}


Try<Nothing> remove(const string& hierarchy, const string& cgroup)
{
  Option<string> error = verify(hierarchy, cgroup);
  if (error.isSome()) {
    return Try<Nothing>::error(error.get());
  }

  Try<vector<string> > cgroups = cgroups::get(hierarchy, cgroup);
  if (cgroups.isError()) {
    return Try<Nothing>::error(
        "Failed to get nested cgroups: " + cgroups.error());
  }

  if (!cgroups.get().empty()) {
    return Try<Nothing>::error("Nested cgroups exist");
  }

  return internal::remove(hierarchy, cgroup);
}


Try<bool> exists(const string& hierarchy, const string& cgroup)
{
  Option<string> error = verify(hierarchy);
  if (error.isSome()) {
    return Try<bool>::error(error.get());
  }

  return os::exists(path::join(hierarchy, cgroup));
}


Try<vector<string> > get(const string& hierarchy, const string& cgroup)
{
  Option<string> error = verify(hierarchy, cgroup);
  if (error.isSome()) {
    return Try<vector<string> >::error(error.get());
  }

  Try<string> hierarchyAbsPath = os::realpath(hierarchy);
  if (hierarchyAbsPath.isError()) {
    return Try<vector<string> >::error(
        "Failed to determine canonical path of " + hierarchy +
        ": " + hierarchyAbsPath.error());
  }

  Try<string> destAbsPath = os::realpath(path::join(hierarchy, cgroup));
  if (destAbsPath.isError()) {
    return Try<vector<string> >::error(
        "Failed to determine canonical path of " +
        path::join(hierarchy, cgroup) + ": " + destAbsPath.error());
  }

  char* paths[] = { const_cast<char*>(destAbsPath.get().c_str()), NULL };

  FTS* tree = fts_open(paths, FTS_NOCHDIR, NULL);
  if (tree == NULL) {
    return Try<vector<string> >::error(
        "Failed to start file system walk: " +
          string(strerror(errno)));
  }

  vector<string> cgroups;

  FTSENT* node;
  while ((node = fts_read(tree)) != NULL) {
    // Use post-order walk here. fts_level is the depth of the traversal,
    // numbered from -1 to N, where the file/dir was found. The traversal root
    // itself is numbered 0. fts_info includes flags for the current node.
    // FTS_DP indicates a directory being visited in postorder.
    if (node->fts_level > 0 && node->fts_info & FTS_DP) {
      string path =
        strings::trim(node->fts_path + hierarchyAbsPath.get().length(), "/");
      cgroups.push_back(path);
    }
  }

  if (errno != 0) {
    return Try<vector<string> >::error(
        "Failed to read a node during the walk: " + string(strerror(errno)));
  }

  if (fts_close(tree) != 0) {
    return Try<vector<string> >::error(
        "Failed to stop a file system walk: " + string(strerror(errno)));
  }

  return cgroups;
}


Try<Nothing> kill(
    const string& hierarchy,
    const string& cgroup,
    int signal)
{
  Option<string> error = verify(hierarchy, cgroup);
  if (error.isSome()) {
    return Try<Nothing>::error(error.get());
  }

  Try<set<pid_t> > pids = tasks(hierarchy, cgroup);
  if (pids.isError()) {
    return Try<Nothing>::error(
        "Failed to get tasks of cgroup: " + pids.error());
  }

  foreach (pid_t pid, pids.get()) {
    if (::kill(pid, signal) == -1) {
      return Try<Nothing>::error(
          "Failed to send " + string(strsignal(signal)) + " to process " +
          stringify(pid) + ": " + strerror(errno));
    }
  }

  return Nothing();
}


Try<string> read(
    const string& hierarchy,
    const string& cgroup,
    const string& control)
{
  Option<string> error = verify(hierarchy, cgroup, control);
  if (error.isSome()) {
    return Try<string>::error(error.get());
  }

  return internal::read(hierarchy, cgroup, control);
}


Try<Nothing> write(
    const string& hierarchy,
    const string& cgroup,
    const string& control,
    const string& value)
{
  Option<string> error = verify(hierarchy, cgroup, control);
  if (error.isSome()) {
    return Try<Nothing>::error(error.get());
  }

  return internal::write(hierarchy, cgroup, control, value);
}


Try<bool> exists(
    const string& hierarchy,
    const string& cgroup,
    const string& control)
{
  Option<string> error = verify(hierarchy, cgroup);
  if (error.isSome()) {
    return Try<bool>::error(error.get());
  }

  return os::exists(path::join(hierarchy, cgroup, control));
}


Try<set<pid_t> > tasks(const string& hierarchy, const string& cgroup)
{
  Try<string> value = cgroups::read(hierarchy, cgroup, "tasks");
  if (value.isError()) {
    return Try<set<pid_t> >::error(
        "Failed to read cgroups control 'tasks': " + value.error());
  }

  // Parse the value read from the control file.
  set<pid_t> pids;
  std::istringstream ss(value.get());
  ss >> std::dec;
  while (!ss.eof()) {
    pid_t pid;
    ss >> pid;

    if (ss.fail()) {
      if (!ss.eof()) {
        return Try<set<pid_t> >::error("Parsing error");
      }
    } else {
      pids.insert(pid);
    }
  }

  return pids;
}


Try<Nothing> assign(const string& hierarchy, const string& cgroup, pid_t pid)
{
  return cgroups::write(hierarchy, cgroup, "tasks", stringify(pid));
}


namespace internal {

#ifndef EFD_SEMAPHORE
#define EFD_SEMAPHORE (1 << 0)
#endif
#ifndef EFD_CLOEXEC
#define EFD_CLOEXEC 02000000
#endif
#ifndef EFD_NONBLOCK
#define EFD_NONBLOCK 04000
#endif

static int eventfd(unsigned int initval, int flags)
{
#ifdef __NR_eventfd2
  return ::syscall(__NR_eventfd2, initval, flags);
#elif defined(__NR_eventfd)
  int fd = ::syscall(__NR_eventfd, initval);
  if (fd == -1) {
    return -1;
  }

  // Manually set CLOEXEC and NONBLOCK.
  if ((flags & EFD_CLOEXEC) != 0) {
    if (os::cloexec(fd).isError()) {
      os::close(fd);
      return -1;
    }
  }

  if ((flags & EFD_NONBLOCK) != 0) {
    if (os::nonblock(fd).isError()) {
      os::close(fd);
      return -1;
    }
  }

  // Return the file descriptor.
  return fd;
#else
#error "The eventfd syscall is not available."
#endif
}


// In cgroups, there is mechanism which allows to get notifications about
// changing status of a cgroup. It is based on Linux eventfd. See more
// information in the kernel documentation ("Notification API"). This function
// will create an eventfd and write appropriate control file to correlate the
// eventfd with a type of event so that users can start polling on the eventfd
// to get notified. It returns the eventfd (file descriptor) if the notifier has
// been successfully registered. This function assumes all the parameters are
// valid. The eventfd is set to be non-blocking.
// @param   hierarchy   Path to the hierarchy root.
// @param   cgroup      Path to the cgroup relative to the hierarchy root.
// @param   control     Name of the control file.
// @param   args        Control specific arguments.
// @return  The eventfd if the operation succeeds.
//          Error if the operation fails.
static Try<int> registerNotifier(
    const string& hierarchy,
    const string& cgroup,
    const string& control,
    const Option<string>& args = Option<string>::none())
{
  int efd = internal::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (efd < 0) {
    return Try<int>::error(
        "Failed to create an eventfd: " + string(strerror(errno)));
  }

  // Open the control file.
  string path = path::join(hierarchy, cgroup, control);
  Try<int> cfd = os::open(path, O_RDWR);
  if (cfd.isError()) {
    os::close(efd);
    return Try<int>::error("Failed to open " + path + ": " + cfd.error());
  }

  // Write the event control file (cgroup.event_control).
  std::ostringstream out;
  out << std::dec << efd << " " << cfd.get();
  if (args.isSome()) {
    out << " " << args.get();
  }
  Try<Nothing> write = internal::write(
      hierarchy, cgroup, "cgroup.event_control", out.str());
  if (write.isError()) {
    os::close(efd);
    os::close(cfd.get());
    return Try<int>::error(
        "Failed to write control 'cgroup.event_control': " + write.error());
  }

  os::close(cfd.get());

  return efd;
}


// Unregister a notifier.
// @param   fd      The eventfd returned by registerNotifier.
// @return  Some if the operation succeeds.
//          Error if the operation fails.
static Try<Nothing> unregisterNotifier(int fd)
{
  return os::close(fd);
}


// The process listening on event notifier. This class is invisible to users.
class EventListener : public Process<EventListener>
{
public:
  EventListener(const string& _hierarchy,
                const string& _cgroup,
                const string& _control,
                const Option<string>& _args)
    : hierarchy(_hierarchy),
      cgroup(_cgroup),
      control(_control),
      args(_args),
      data(0) {}

  virtual ~EventListener() {}

  Future<uint64_t> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop the listener if no one cares. Note that here we explicitly specify
    // the type of the terminate function because it is an overloaded function.
    // The compiler complains if we do not do it.
    promise.future().onDiscarded(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    // Register an eventfd "notifier" for the given control.
    Try<int> fd = internal::registerNotifier(hierarchy, cgroup, control, args);
    if (fd.isError()) {
      promise.fail("Failed to register notification eventfd: " + fd.error());
      terminate(self());
      return;
    }

    // Remember the opened event file descriptor.
    eventfd = fd.get();

    // Perform nonblocking read on the event file. The nonblocking read will
    // start polling on the event file until it becomes readable. If we can
    // successfully read 8 bytes (sizeof uint64_t) from the event file, it
    // indicates an event has occurred.
    reading = io::read(eventfd.get(), &data, sizeof(data));
    reading.onAny(defer(self(), &EventListener::notified, lambda::_1));
  }

  virtual void finalize()
  {
    // Discard the nonblocking read.
    reading.discard();

    // Unregister the eventfd if needed.
    if (eventfd.isSome()) {
      Try<Nothing> unregister = internal::unregisterNotifier(eventfd.get());
      if (unregister.isError()) {
        LOG(ERROR) << "Failed to unregistering eventfd: " << unregister.error();
      }
    }
  }

private:
  // This function is called when the nonblocking read on the eventfd has
  // result, either because the event has happened, or an error has occurred.
  void notified(const Future<size_t>&)
  {
    // Ignore this function if the promise is no longer pending.
    if (!promise.future().isPending()) {
      return;
    }

    // Since the future reading can only be discarded when the promise is no
    // longer pending, we shall never see a discarded reading here because of
    // the check in the beginning of the function.
    CHECK(!reading.isDiscarded());

    if (reading.isFailed()) {
      promise.fail("Failed to read eventfd: " + reading.failure());
    } else {
      if (reading.get() == sizeof(data)) {
        promise.set(data);
      } else {
        promise.fail("Read less than expected");
      }
    }

    terminate(self());
  }

  const string hierarchy;
  const string cgroup;
  const string control;
  const Option<string> args;
  Promise<uint64_t> promise;
  Future<size_t> reading;
  Option<int> eventfd;  // The eventfd if opened.
  uint64_t data; // The data read from the eventfd.
};

} // namespace internal {


Future<uint64_t> listen(
    const string& hierarchy,
    const string& cgroup,
    const string& control,
    const Option<string>& args)
{
  Option<string> error = verify(hierarchy, cgroup, control);
  if (error.isSome()) {
    return Future<uint64_t>::failed(error.get());
  }

  internal::EventListener* listener =
    new internal::EventListener(hierarchy, cgroup, control, args);
  Future<uint64_t> future = listener->future();
  spawn(listener, true);
  return future;
}


namespace internal {


// The process that freezes or thaws the cgroup.
class Freezer : public Process<Freezer>
{
public:
  Freezer(const string& _hierarchy,
          const string& _cgroup,
          const string& _action,
          const Duration& _interval,
          unsigned int _retries = FREEZE_RETRIES)
    : hierarchy(_hierarchy),
      cgroup(_cgroup),
      action(_action),
      interval(_interval),
      retries(_retries) {}

  virtual ~Freezer() {}

  // Return a future indicating the state of the freezer.
  Future<bool> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop the process if no one cares.
    promise.future().onDiscarded(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    CHECK(interval >= Seconds(0));

    // Start the action.
    CHECK(action == "FREEZE" || action == "THAW");
    if (action == "FREEZE") {
      freeze();
    } else if (action == "THAW") {
      thaw();
    }
  }

private:
  void freeze()
  {
    LOG(INFO) << "Trying to freeze cgroup " << path::join(hierarchy, cgroup);

    Try<Nothing> write = internal::write(
        hierarchy, cgroup, "freezer.state", "FROZEN");

    if (write.isError()) {
      promise.fail("Failed to write control 'freezer.state': " + write.error());
      terminate(self());
    } else {
      watchFrozen();
    }
  }

  void thaw()
  {
    LOG(INFO) << "Trying to thaw cgroup " << path::join(hierarchy, cgroup);

    Try<Nothing> write = internal::write(
        hierarchy, cgroup, "freezer.state", "THAWED");

    if (write.isError()) {
      promise.fail("Failed to write control 'freezer.state': " + write.error());
      terminate(self());
    } else {
      watchThawed();
    }
  }

  void watchFrozen(unsigned int attempt = 0)
  {
    Try<string> state = internal::read(hierarchy, cgroup, "freezer.state");

    if (state.isError()) {
      promise.fail("Failed to read control 'freezer.state': " + state.error());
      terminate(self());
      return;
    }

    if (strings::trim(state.get()) == "FROZEN") {
      LOG(INFO) << "Successfully froze cgroup " << path::join(hierarchy, cgroup)
                << " after " << attempt + 1 << " attempts";
      promise.set(true);
      terminate(self());
      return;
    } else if (strings::trim(state.get()) == "FREEZING") {
      // The freezer.state is in FREEZING state. This is because not all the
      // processes in the given cgroup can be frozen at the moment. The main
      // cause is that some of the processes are in stopped/traced state ('T'
      // state shown in ps command). It is likely that the freezer.state keeps
      // in FREEZING state if these stopped/traced processes are not resumed.
      // Therefore, here we send SIGCONT to those stopped/traced processes to
      // make sure that the freezer can finish.
      // TODO(jieyu): This code can be removed in the future as the newer
      // version of the kernel solves this problem (e.g. Linux-3.2.0).
      Try<set<pid_t> > pids = tasks(hierarchy, cgroup);
      if (pids.isError()) {
        promise.fail("Failed to get tasks of cgroup: " + pids.error());
        terminate(self());
        return;
      }

      // We don't need to worry about the race condition here as it is not
      // possible to add new processes into this cgroup or remove processes from
      // this cgroup when freezer.state is in FREEZING state.
      foreach (pid_t pid, pids.get()) {
        Try<proc::ProcessStatistics> stat = proc::stat(pid);
        if (stat.isError()) {
          promise.fail("Failed to get process statistics: " + stat.error());
          terminate(self());
          return;
        }

        // Check whether the process is in stopped/traced state.
        if (stat.get().state == 'T') {
          // Send a SIGCONT signal to the process.
          if (::kill(pid, SIGCONT) == -1) {
            promise.fail(
                "Failed to send SIGCONT to process " + stringify(pid) +
                ": " + string(strerror(errno)));
            terminate(self());
            return;
          }
        }
      }

      if (attempt > retries) {
        LOG(WARNING) << "Unable to freeze " << path::join(hierarchy, cgroup)
                     << " within " << retries + 1 << " attempts";
        promise.set(false);
        terminate(self());
        return;
      }

      // Retry the freezing operation.
      Try<Nothing> write = internal::write(
          hierarchy, cgroup, "freezer.state", "FROZEN");

      if (write.isError()) {
        promise.fail(
            "Failed to write control 'freezer.state': " + write.error());
        terminate(self());
        return;
      }

      // Not done yet, keep watching (and possibly retrying).
      delay(interval, self(), &Freezer::watchFrozen, attempt + 1);
    } else {
      LOG(FATAL) << "Unexpected state: " << strings::trim(state.get());
    }
  }

  void watchThawed()
  {
    Try<string> state = internal::read(hierarchy, cgroup, "freezer.state");

    if (state.isError()) {
      promise.fail("Failed to read control 'freezer.state': " + state.error());
      terminate(self());
      return;
    }

    if (strings::trim(state.get()) == "THAWED") {
      LOG(INFO) << "Successfully thawed " << path::join(hierarchy, cgroup);
      promise.set(true);
      terminate(self());
    } else if (strings::trim(state.get()) == "FROZEN") {
      // Not done yet, keep watching.
      delay(interval, self(), &Freezer::watchThawed);
    } else {
      LOG(FATAL) << "Unexpected state: " << strings::trim(state.get());
    }
  }

  const string hierarchy;
  const string cgroup;
  const string action;
  const Duration interval;
  const unsigned int retries;
  Promise<bool> promise;
};

} // namespace internal {


Future<bool> freeze(
    const string& hierarchy,
    const string& cgroup,
    const Duration& interval,
    unsigned int retries)
{
  Option<string> error = verify(hierarchy, cgroup, "freezer.state");
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  }

  if (interval < Seconds(0)) {
    return Future<bool>::failed("Interval should be non-negative");
  }

  // Check the current freezer state.
  Try<string> state = internal::read(hierarchy, cgroup, "freezer.state");
  if (state.isError()) {
    return Future<bool>::failed(
        "Failed to read control 'freezer.state': " + state.error());
  } else if (strings::trim(state.get()) == "FROZEN") {
    // Immediately return success.
    return true;
  }

  internal::Freezer* freezer =
    new internal::Freezer(hierarchy, cgroup, "FREEZE", interval, retries);
  Future<bool> future = freezer->future();
  spawn(freezer, true);
  return future;
}


Future<bool> thaw(
    const string& hierarchy,
    const string& cgroup,
    const Duration& interval)
{
  Option<string> error = verify(hierarchy, cgroup, "freezer.state");
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  }

  if (interval < Seconds(0)) {
    return Future<bool>::failed("Interval should be non-negative");
  }

  // Check the current freezer state.
  Try<string> state = internal::read(hierarchy, cgroup, "freezer.state");
  if (state.isError()) {
    return Future<bool>::failed(
        "Failed to read control 'freezer.state': " + state.error());
  } else if (strings::trim(state.get()) == "THAWED") {
    // Immediately return success.
    return true;
  }

  internal::Freezer* freezer =
    new internal::Freezer(hierarchy, cgroup, "THAW", interval);
  Future<bool> future = freezer->future();
  spawn(freezer, true);
  return future;
}


namespace internal {

// The process used to wait for a cgroup to become empty (no task in it).
class EmptyWatcher: public Process<EmptyWatcher>
{
public:
  EmptyWatcher(const string& _hierarchy,
               const string& _cgroup,
               const Duration& _interval,
               unsigned int _retries = EMPTY_WATCHER_RETRIES)
    : hierarchy(_hierarchy),
      cgroup(_cgroup),
      interval(_interval),
      retries(_retries) {}

  virtual ~EmptyWatcher() {}

  // Return a future indicating the state of the watcher.
  // There are three outcomes:
  //   1. true:  the cgroup became empty.
  //   2. false: the cgroup did not become empty within the retry limit.
  //   3. error: invalid arguments, or an unexpected error occured.
  Future<bool> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    CHECK(interval >= Seconds(0));

    check();
  }

private:
  void check(unsigned int attempt = 0)
  {
    Try<set<pid_t> > pids = tasks(hierarchy, cgroup);
    if (pids.isError()) {
      promise.fail("Failed to get tasks of cgroup: " + pids.error());
      terminate(self());
      return;
    }

    if (pids.get().empty()) {
      promise.set(true);
      terminate(self());
      return;
    } else {
      if (attempt > retries) {
        promise.set(false);
        terminate(self());
        return;
      }

      // Re-check needed.
      delay(interval, self(), &EmptyWatcher::check, attempt + 1);
    }
  }

  const string hierarchy;
  const string cgroup;
  const Duration interval;
  const unsigned int retries;
  Promise<bool> promise;
};


// The process used to atomically kill all tasks in a cgroup.
class TasksKiller : public Process<TasksKiller>
{
public:
  TasksKiller(const string& _hierarchy,
              const string& _cgroup,
              const Duration& _interval)
    : hierarchy(_hierarchy),
      cgroup(_cgroup),
      interval(_interval) {}

  virtual ~TasksKiller() {}

  // Return a future indicating the state of the killer.
  Future<bool> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
          static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    CHECK(interval >= Seconds(0));

    killTasks();
  }

  virtual void finalize()
  {
    // Cancel the chain of operations if the user discards the future.
    if (promise.future().isDiscarded()) {
      chain.discard();
    }
  }

private:
  void killTasks() {
    lambda::function<Future<bool>(const bool&)>
      funcFreeze = defer(self(), &Self::freeze);
    lambda::function<Future<Nothing>(const bool&)>
      funcKill = defer(self(), &Self::kill);
    lambda::function<Future<bool>(const Nothing&)>
      funcThaw = defer(self(), &Self::thaw);
    lambda::function<Future<bool>(const bool&)>
      funcEmpty = defer(self(), &Self::empty);

    // Chain together the steps needed to kill the tasks. Note that we
    // ignore he return values of freeze, kill, and thaw because,
    // provided there are no errors, we'll just retry the chain as
    // long as tasks still exist.
    chain = Future<bool>(true)
      .then(funcFreeze)   // Freeze the cgroup.
      .then(funcKill)     // Send kill signals to all tasks in the cgroup.
      .then(funcThaw)     // Thaw the cgroup to let kill signals be received.
      .then(funcEmpty);   // Wait until no task is in the cgroup.

    chain.onAny(defer(self(), &Self::finished, lambda::_1));
  }

  Future<bool> freeze()
  {
    return cgroups::freeze(hierarchy, cgroup, interval);
  }

  Future<Nothing> kill()
  {
    Try<Nothing> kill = cgroups::kill(hierarchy, cgroup, SIGKILL);
    if (kill.isError()) {
      return Future<Nothing>::failed(kill.error());
    }
    return Nothing();
  }

  Future<bool> thaw()
  {
    return cgroups::thaw(hierarchy, cgroup, interval);
  }

  Future<bool> empty()
  {
    EmptyWatcher* watcher = new EmptyWatcher(hierarchy, cgroup, interval);
    Future<bool> future = watcher->future();
    spawn(watcher, true);
    return future;
  }

  void finished(const Future<bool>& empty)
  {
    CHECK(!empty.isPending() && !empty.isDiscarded());
    if (empty.isFailed()) {
      promise.fail(empty.failure());
      terminate(self());
    } else if (empty.get()) {
      promise.set(true);
      terminate(self());
    } else {
      // The cgroup was not empty after the retry limit.
      // We need to re-attempt the freeze/kill/thaw/watch chain.
      killTasks();
    }
  }

  const string hierarchy;
  const string cgroup;
  const Duration interval;
  Promise<bool> promise;
  Future<bool> chain; // Used to discard the "chain" of operations.
};


// The process used to destroy a cgroup.
class Destroyer : public Process<Destroyer>
{
public:
  Destroyer(const string& _hierarchy,
            const vector<string>& _cgroups,
            const Duration& _interval)
    : hierarchy(_hierarchy),
      cgroups(_cgroups),
      interval(_interval) {}

  virtual ~Destroyer() {}

  // Return a future indicating the state of the destroyer.
  Future<bool> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscarded(lambda::bind(
          static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    CHECK(interval >= Seconds(0));

    // Kill tasks in the given cgroups in parallel. Use collect mechanism to
    // wait until all kill processes finish.
    foreach (const string& cgroup, cgroups) {
      internal::TasksKiller* killer =
        new internal::TasksKiller(hierarchy, cgroup, interval);
      killers.push_back(killer->future());
      spawn(killer, true);
    }

    collect(killers)
      .onAny(defer(self(), &Destroyer::killed, lambda::_1));
  }

  virtual void finalize()
  {
    // Cancel the operation if the user discards the future.
    if (promise.future().isDiscarded()) {
      discard<bool>(killers);
    }
  }

private:
  void killed(const Future<list<bool> >& kill)
  {
    CHECK(!kill.isPending() && !kill.isDiscarded());
    if (kill.isReady()) {
      remove();
    } else if (kill.isFailed()) {
      promise.fail("Failed to kill tasks in nested cgroups: " + kill.failure());
      terminate(self());
    }
  }

  void remove()
  {
    foreach (const string& cgroup, cgroups) {
      Try<Nothing> remove = internal::remove(hierarchy, cgroup);
      if (remove.isError()) {
        promise.fail(
            "Failed to remove cgroup " + cgroup + ": " + remove.error());
        terminate(self());
        return;
      }
    }

    promise.set(true);
    terminate(self());
  }

  const string hierarchy;
  const vector<string> cgroups;
  const Duration interval;
  Promise<bool> promise;

  // The killer processes used to atomically kill tasks in each cgroup.
  list<Future<bool> > killers;
};

} // namespace internal {


Future<bool> destroy(
    const string& hierarchy,
    const string& cgroup,
    const Duration& interval)
{
  Option<string> error = verify(hierarchy, cgroup, "freezer.state");
  if (error.isSome()) {
    return Future<bool>::failed(error.get());
  }

  if (interval < Seconds(0)) {
    return Future<bool>::failed("Interval should be non-negative");
  }

  // Construct the vector of cgroups to destroy.
  Try<vector<string> > cgroups = cgroups::get(hierarchy, cgroup);
  if (cgroups.isError()) {
    return Future<bool>::failed(
        "Failed to get nested cgroups: " + cgroups.error());
  }

  vector<string> toDestroy = cgroups.get();
  if (cgroup != "/") {
    toDestroy.push_back(cgroup);
  }

  internal::Destroyer* destroyer =
    new internal::Destroyer(hierarchy, toDestroy, interval);
  Future<bool> future = destroyer->future();
  spawn(destroyer, true);
  return future;
}

} // namespace cgroups {
