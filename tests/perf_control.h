#pragma once

/*
 * Lets perf count one region of a run (Fig. 7), not the whole
 * process, which reading the input dominates. perf starts with its events
 * off (perf stat -D -1) and listens on an fd pair (--control fd:CTL,ACK):
 * "enable" and "disable" written to CTL toggle counting, and perf answers
 * "ack\n" on ACK. The driver gets the fds from -pcf and -pcaf; with either
 * 0 every call here is a no-op.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>

struct perf_control {
	static inline int ctl_fd = 0;
	static inline int ack_fd = 0;
	/* the region to count (-pcr); empty counts every region */
	static inline std::string region;
	/* only the outermost of nested regions toggles perf */
	static inline int depth = 0;

	static bool active() noexcept
	{
		return ctl_fd != 0 && ack_fd != 0;
	}

	static void install(int ctl, int ack, std::string_view name)
	{
		ctl_fd = ctl;
		ack_fd = ack;
		region = std::string(name);
	}

	static bool matches(std::string_view name) noexcept
	{
		return active() && (region.empty() || region == name);
	}

	static void enable() noexcept
	{
		if (depth++ == 0) {
			send("enable");
		}
	}

	static void disable() noexcept
	{
		if (--depth == 0) {
			send("disable");
		}
	}

private:
	static void send(char const *cmd) noexcept
	{
		/* perf compares with strncmp, so the NUL may go too */
		if (::write(ctl_fd, cmd, std::strlen(cmd) + 1) < 0) {
			std::fprintf(stderr, "perf ctl: write(%s) failed\n", cmd);
			return;
		}
		/* drain the reply, or the next command reads a stale one */
		char ack[16] = { 0 };
		ssize_t const r = ::read(ack_fd, ack, sizeof(ack) - 1);
		if (r < 0) {
			std::fprintf(stderr, "perf ctl: no ack for %s\n", cmd);
			return;
		}
		ack[r] = '\0';
		if (std::strncmp(ack, "ack", 3) != 0) {
			std::fprintf(stderr, "perf ctl: unexpected ack for %s: '%s'\n",
						 cmd, ack);
		}
	}
};

/* counts while alive, if NAME is the region -pcr chose */
struct perf_region {
	explicit perf_region(std::string_view name) noexcept
		: armed(perf_control::matches(name))
	{
		if (armed) {
			perf_control::enable();
		}
	}

	~perf_region() noexcept
	{
		if (armed) {
			perf_control::disable();
		}
	}

	perf_region(perf_region const &) = delete;
	perf_region &operator=(perf_region const &) = delete;

private:
	bool armed;
};
