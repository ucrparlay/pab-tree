#pragma once

// NOTE: gates a `perf` session so the hardware counters only cover one chosen
// region of code (a single Build / BatchInsert / BatchDelete call) instead of
// the whole process -- otherwise the counts are dominated by reading the
// 100M-entry input file and by the untimed setup Build.
//
// Mechanism: perf is launched with `-D -1` (start with events disabled) and
// `--control fd:<ctl>,<ack>`, where both fds are inherited pipes/fifos created
// by the driver script. Writing "enable" / "disable" to the control fd toggles
// counting; perf replies "ack\n" on the ack fd, which we must drain to stay in
// sync. The fd numbers arrive via `-pcf` / `-pcaf`.
//
// Region filtering matters because a single run walks several guarded regions:
// `-t 1` (batch insert) first builds the tree via BuildTree, which is itself
// guarded. perf accumulates over every enabled interval, so without `-pcr` the
// reported counts would be build+insert summed. Pass `-pcr build|insert|delete`
// to arm exactly one.
//
// When either fd is 0 (the default, i.e. not running under the perf script)
// every entry point below is a no-op, so normal timing runs are unaffected.

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>

struct PerfControl
{
  // NOTE: set once from Options in Run(); 0 means "not under perf".
  static inline int ctl_fd = 0;
  static inline int ack_fd = 0;
  // NOTE: which region to count. Empty means "every region", which sums them.
  static inline std::string region = "";
  // NOTE: guards against a nested guarded region disabling counting early.
  static inline int depth = 0;

  static bool Active() noexcept { return ctl_fd != 0 && ack_fd != 0; }

  static void Install(int ctl, int ack, std::string_view sel)
  {
    ctl_fd = ctl;
    ack_fd = ack;
    region = std::string(sel);
  }

  static bool Matches(std::string_view name) noexcept
  {
    return Active() && (region.empty() || region == name);
  }

  static void Enable() noexcept
  {
    if (depth++ == 0) {
      Send("enable");
    }
  }

  static void Disable() noexcept
  {
    if (--depth == 0) {
      Send("disable");
    }
  }

private:
  static void Send(std::string_view cmd) noexcept
  {
    if (!Active()) {
      return;
    }
    // NOTE: perf matches the command with strncmp, so the trailing NUL is fine.
    if (::write(ctl_fd, cmd.data(), cmd.size() + 1) < 0) {
      std::fprintf(stderr, "perf ctl: write(%s) failed\n", cmd.data());
      return;
    }
    // NOTE: perf replies "ack\n"; drain it so the next command does not read a
    // stale reply. Terminated at the byte count actually read rather than
    // assuming a fixed length.
    char ack[16] = { 0 };
    ssize_t const r = ::read(ack_fd, ack, sizeof(ack) - 1);
    if (r < 0) {
      std::fprintf(stderr, "perf ctl: read ack for %s failed\n", cmd.data());
      return;
    }
    ack[r] = '\0';
    if (std::strncmp(ack, "ack", 3) != 0) {
      std::fprintf(
        stderr, "perf ctl: unexpected ack for %s: '%s'\n", cmd.data(), ack);
    }
  }
};

// NOTE: scope guard -- counting is on for the lifetime of the object, but only
// if this region is the one selected by `-pcr`. Put one inside the timed lambda
// of time_loop so the untimed init/teardown phases (Build, DeleteTree,
// ParseInput) stay out of the counts.
struct PerfRegion
{
  explicit PerfRegion(std::string_view name) noexcept
    : armed_(PerfControl::Matches(name))
  {
    if (armed_) {
      PerfControl::Enable();
    }
  }

  ~PerfRegion() noexcept
  {
    if (armed_) {
      PerfControl::Disable();
    }
  }

  PerfRegion(PerfRegion const&) = delete;
  PerfRegion& operator=(PerfRegion const&) = delete;

private:
  bool armed_;
};
