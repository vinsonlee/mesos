#ifndef __PROCESS_PROFILER_HPP__
#define __PROCESS_PROFILER_HPP__

#include <glog/logging.h>

#include <gperftools/profiler.h>

#include <string>

#include <process/future.hpp>
#include <process/http.hpp>
#include <process/process.hpp>

#include <stout/format.hpp>

namespace process {

const std::string PROFILE_FILE = "perftools.out";

class Profiler : public Process<Profiler>
{
public:
  Profiler() : ProcessBase("profiler"), started(false) {}

  virtual ~Profiler() {}

protected:
  virtual void initialize()
  {
    route("/start", &Profiler::start);
    route("/stop", &Profiler::stop);
  }

private:
  // HTTP endpoints.

  // Starts the profiler. There are no request parameters.
  Future<http::Response> start(const http::Request& request)
  {
    if (started) {
      return http::BadRequest("Profiler already started.\n");
    }

    LOG(INFO) << "Starting Profiler";

    // WARNING: If using libunwind < 1.0.1, profiling should not be used, as
    // there are reports of crashes.
    // WARNING: If using libunwind 1.0.1, profiling should not be turned on
    // when it's possible for new threads to be created.
    // This may cause a deadlock. The workaround used in libprocess is described
    // here:
    // https://groups.google.com/d/topic/google-perftools/Df10Uy4Djrg/discussion
    // NOTE: We have not tested this with libunwind > 1.0.1.
    if (!ProfilerStart(PROFILE_FILE.c_str())) {
      std::string error =
        strings::format("Failed to start profiler: %s", strerror(errno)).get();
      LOG(ERROR) << error;
      return http::InternalServerError(error);
    }

    started = true;
    return http::OK("Profiler started.\n");
  }

  // Stops the profiler. There are no request parameters.
  // This returns the profile output, it will also remain present
  // in the working directory.
  Future<http::Response> stop(const http::Request& request)
  {
    if (!started) {
      return http::BadRequest("Profiler not running.\n");
    }

    LOG(INFO) << "Stopping Profiler";

    ProfilerStop();

    http::OK response;
    response.type = response.PATH;
    response.path = "perftools.out";
    response.headers["Content-Type"] = "application/octet-stream";
    response.headers["Content-Disposition"] =
      strings::format("attachment; filename=%s", PROFILE_FILE).get();

    started = false;
    return response;
  }

  bool started;
};

} // namespace process {

#endif // __PROCESS_PROCESS_HPP__
