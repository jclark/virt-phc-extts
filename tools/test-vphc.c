// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/ptp_clock.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCKFD
#define CLOCKFD 3
#endif
#define FD_TO_CLOCKID(fd) ((~(clockid_t)(fd) << 3) | CLOCKFD)
#define NS_PER_SEC 1000000000LL

static int ptp_fd = -1, edge_fd = -1;
static clockid_t ptp_clock;

static void fail(const char *what)
{
	fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
	exit(EXIT_FAILURE);
}

static void require(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "FAIL: %s\n", what);
		exit(EXIT_FAILURE);
	}
}

static int64_t ts_ns(struct ptp_clock_time t)
{
	return (int64_t)t.sec * NS_PER_SEC + t.nsec;
}

static int64_t raw_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts))
		fail("clock_gettime(CLOCK_MONOTONIC_RAW)");
	return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

static int64_t clock_ns(clockid_t id)
{
	struct timespec ts;
	if (clock_gettime(id, &ts))
		fail("clock_gettime(PHC)");
	return (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec;
}

static int schedule_write(const char *buf)
{
	size_t n = strlen(buf);
	ssize_t wrote = pwrite(edge_fd, buf, n, 0);
	if (wrote < 0)
		return -errno;
	return (size_t)wrote == n ? 0 : -EIO;
}

static int edge_write(int64_t edge)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%lld\n", (long long)edge);
	return schedule_write(buf);
}

static void pair_write(int64_t a, int64_t b, char separator)
{
	char buf[128];
	snprintf(buf, sizeof(buf), "%lld%c%lld\n", (long long)a, separator, (long long)b);
	require(schedule_write(buf) == 0, "write two-edge schedule");
}

static void expect_schedule(const char *expected)
{
	char buf[1024];
	ssize_t n = pread(edge_fd, buf, sizeof(buf) - 1, 0);
	if (n < 0)
		fail("read schedule");
	buf[n] = '\0';
	require(!strcmp(buf, expected), "pending schedule differs from expected");
}

static void cleanup(void)
{
	if (edge_fd >= 0)
		(void)schedule_write("\n");
	if (ptp_fd >= 0) {
		struct ptp_extts_request req = { .index = 0, .flags = 0 };
		struct ptp_pin_desc pin = { .index = 0, .func = PTP_PF_NONE };
		struct timex tx = { .modes = ADJ_FREQUENCY, .freq = 0 };
		(void)ioctl(ptp_fd, PTP_EXTTS_REQUEST2, &req);
		(void)clock_adjtime(ptp_clock, &tx);
		(void)ioctl(ptp_fd, PTP_PIN_SETFUNC2, &pin);
	}
	if (edge_fd >= 0)
		close(edge_fd);
	if (ptp_fd >= 0)
		close(ptp_fd);
}

static void expect_edge_error(int64_t edge, int expected, const char *what)
{
	int rc = edge_write(edge);
	require(rc == -expected, what);
}

static struct ptp_extts_event read_event(int timeout_ms)
{
	struct pollfd pfd = { .fd = ptp_fd, .events = POLLIN };
	struct ptp_extts_event ev;
	int rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0)
		fail("poll(PHC events)");
	require(rc > 0 && (pfd.revents & POLLIN), "timed out waiting for edge event");
	ssize_t n = read(ptp_fd, &ev, sizeof(ev));
	if (n < 0)
		fail("read(PHC event)");
	require(n == sizeof(ev), "short PHC event read");
	return ev;
}

static void expect_no_event(int timeout_ms)
{
	struct pollfd pfd = { .fd = ptp_fd, .events = POLLIN };
	int rc = poll(&pfd, 1, timeout_ms);
	if (rc < 0)
		fail("poll(disabled PHC)");
	require(rc == 0, "disabled input produced an event");
}

static void select_edges(unsigned int edges)
{
	struct ptp_extts_request req = {
		.index = 0,
		.flags = edges ? PTP_ENABLE_FEATURE | edges : 0,
	};
	if (ioctl(ptp_fd, PTP_EXTTS_REQUEST2, &req))
		fail("select EXTTs edges");
}

static void set_enabled(int enabled)
{
	select_edges(enabled ? PTP_RISING_EDGE : 0);
}

static void assign_pin(void)
{
	struct ptp_pin_desc pin = {
		.index = 0,
		.func = PTP_PF_EXTTS,
		.chan = 0,
	};
	if (ioctl(ptp_fd, PTP_PIN_SETFUNC2, &pin))
		fail("assign pin to EXTTs channel 0");
}

static void check_offset_apis(int no_cross)
{
	struct ptp_sys_offset_precise precise = { 0 };
	struct ptp_sys_offset_extended ext = { .n_samples = 3 };
	int64_t before, after, phc;

	errno = 0;
	if (!ioctl(ptp_fd, PTP_SYS_OFFSET_PRECISE2, &precise)) {
		require(!no_cross, "precise cross timestamp unexpectedly succeeded");
		before = raw_ns();
		require(llabs(ts_ns(precise.sys_monoraw) - before) < 1000000000LL,
			"precise raw system timestamp is implausible");
		phc = clock_ns(ptp_clock);
		require(llabs(ts_ns(precise.device) - phc) < 1000000000LL,
			"precise device timestamp disagrees with PHC read");
	} else if (no_cross) {
		require(errno == EOPNOTSUPP,
			"precise cross timestamp did not fail with EOPNOTSUPP");
	} else {
		fail("PTP_SYS_OFFSET_PRECISE");
	}

	if (ioctl(ptp_fd, PTP_SYS_OFFSET_EXTENDED2, &ext))
		fail("PTP_SYS_OFFSET_EXTENDED");
	require(ext.n_samples == 3, "extended ioctl returned wrong sample count");
	for (unsigned int i = 0; i < ext.n_samples; i++) {
		int64_t pre = ts_ns(ext.ts[i][0]);
		int64_t device = ts_ns(ext.ts[i][1]);
		int64_t post = ts_ns(ext.ts[i][2]);
		require(pre <= post, "extended system bracket is reversed");
		before = clock_ns(ptp_clock);
		after = clock_ns(ptp_clock);
		require(device >= before - 1000000000LL && device <= after + 1000000000LL,
			"extended device timestamp disagrees with PHC read");
	}
}

static void check_set_adjtime(void)
{
	struct timespec ts;
	struct timex tx = { 0 };
	int64_t target, actual;

	if (clock_gettime(ptp_clock, &ts))
		fail("read PHC before settime");
	target = (int64_t)ts.tv_sec * NS_PER_SEC + ts.tv_nsec + 2000000000LL;
	ts.tv_sec = target / NS_PER_SEC;
	ts.tv_nsec = target % NS_PER_SEC;
	if (clock_settime(ptp_clock, &ts))
		fail("clock_settime(PHC)");
	actual = clock_ns(ptp_clock);
	require(llabs(actual - target) < 1000000,
		"settime did not move PHC to requested time");
	tx.modes = ADJ_SETOFFSET | ADJ_NANO;
	tx.time.tv_sec = 0;
	tx.time.tv_usec = 250000000;
	if (clock_adjtime(ptp_clock, &tx) < 0)
		fail("clock_adjtime(PHC offset)");
	actual = clock_ns(ptp_clock);
	require(actual >= target + 249000000LL && actual <= target + 251000000LL,
		"adjtime did not move PHC by 250 ms");
}

static void check_pair(int64_t expected_interval, const char *what)
{
	int64_t edge0 = raw_ns() + 100000000LL;
	int64_t edge1 = edge0 + NS_PER_SEC;
	struct ptp_extts_event a, b;
	pair_write(edge0, edge1, ' ');
	a = read_event(3000);
	b = read_event(3000);
	require(a.index == 0 && b.index == 0, "event has wrong EXTTs channel");
	printf("edge raw=%lld phc=%lld; raw=%lld phc=%lld; interval=%lld ns\n",
	       (long long)edge0, (long long)ts_ns(a.t),
	       (long long)edge1, (long long)ts_ns(b.t),
	       (long long)(ts_ns(b.t) - ts_ns(a.t)));
	require(llabs((ts_ns(b.t) - ts_ns(a.t)) - expected_interval) <= 1, what);
}

static void check_step_between_edges(long drift)
{
	int64_t first = raw_ns() + 100000000LL;
	struct timex tx = { .modes = ADJ_SETOFFSET | ADJ_NANO };
	struct ptp_extts_event a, b;

	pair_write(first, first + NS_PER_SEC, ' ');
	a = read_event(3000);
	tx.time.tv_usec = 250000000;
	if (clock_adjtime(ptp_clock, &tx) < 0)
		fail("step clock between pending edges");
	b = read_event(3000);
	require(llabs(ts_ns(b.t) - ts_ns(a.t) - NS_PER_SEC - drift - 250000000LL) <= 1,
		"pending edge did not reflect clock step at its nominal time");
}

static void check_schedule(void)
{
	char original[128], buf[1024];
	int64_t a = raw_ns() + 1000000000LL, b = a + 100000000LL;
	size_t len = 0;
	const char *bad[] = {",", "1,,2", "x", "-1", "+1", "2 1", "1 1", "2,1"};

	snprintf(original, sizeof(original), "%lld,%lld\n", (long long)a, (long long)b);
	require(schedule_write(original) == 0, "schedule pulse");
	expect_schedule(original);
	/* An omitted falling edge is accepted and normalized to a bare rise. */
	snprintf(buf, sizeof(buf), "%lld,\n", (long long)a);
	require(schedule_write(buf) == 0, "rising-only comma token rejected");
	snprintf(buf, sizeof(buf), "%lld\n", (long long)a);
	expect_schedule(buf);
	snprintf(buf, sizeof(buf), ",%lld\n", (long long)b);
	require(schedule_write(buf) == 0, "falling-only token rejected");
	expect_schedule(buf);
	require(schedule_write(original) == 0, "restore paired schedule");
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		require(schedule_write(bad[i]) == -EINVAL, "invalid schedule accepted");
		expect_schedule(original);
	}
	require(schedule_write("18446744073709551616") == -ERANGE,
		"overflowing deadline accepted");
	expect_schedule(original);
	errno = 0;
	require(pwrite(edge_fd, "1\0 2", 4, 0) < 0 && errno == EINVAL,
		"embedded NUL accepted");
	expect_schedule(original);
	for (int i = 0; i < 17; i++)
		len += snprintf(buf + len, sizeof(buf) - len, "%lld ", (long long)(a + i));
	require(schedule_write(buf) == -ENOSPC, "oversized future schedule accepted");
	expect_schedule(original);
	expect_edge_error(raw_ns() + 4000000000LL, EINVAL, "too-far deadline accepted");
	expect_schedule(original);
	require(schedule_write("0 1 2\n") == 0, "past edges rejected");
	expect_schedule("\n");
	require(schedule_write(original) == 0, "replace schedule");
	require(schedule_write("\n") == 0, "clear schedule");
	expect_schedule("\n");
	/* Expired entries in a read/append/write must neither fail nor replay. */
	a = raw_ns() + 100000000LL;
	b = a + 300000000LL;
	pair_write(a, b, ' ');
	(void)read_event(1500);
	pair_write(a, b, ' ');
	snprintf(buf, sizeof(buf), "%lld\n", (long long)b);
	expect_schedule(buf);
	(void)read_event(1500);
	expect_no_event(100);
	/* Keep the falling half when the rising half has already occurred. */
	set_enabled(0);
	a = raw_ns() - 1;
	b = a + 300000000LL;
	pair_write(a, b, ',');
	snprintf(buf, sizeof(buf), ",%lld\n", (long long)b);
	expect_schedule(buf);
	require(schedule_write("\n") == 0, "clear falling edge");
	set_enabled(1);
}

static void check_polarity(long drift)
{
	unsigned int selections[] = { PTP_RISING_EDGE, PTP_FALLING_EDGE,
		PTP_RISING_EDGE | PTP_FALLING_EDGE };

	for (size_t i = 0; i < sizeof(selections) / sizeof(selections[0]); i++) {
		int64_t raw = raw_ns();
		int64_t phc = clock_ns(ptp_clock);
		int64_t rise = raw + 100000000LL, fall = rise + 100000000LL;
		int64_t dt = selections[i] == PTP_FALLING_EDGE ? fall - raw : rise - raw;
		struct ptp_extts_event a, b;

		select_edges(selections[i]);
		pair_write(rise, fall, ',');
		a = read_event(1500);
		require(llabs(ts_ns(a.t) - phc - dt - dt * drift / NS_PER_SEC) < 1000000,
			"wrong polarity captured");
		if (selections[i] == (PTP_RISING_EDGE | PTP_FALLING_EDGE)) {
			b = read_event(1500);
			require(llabs(ts_ns(b.t) - ts_ns(a.t) - 100000000LL - drift / 10) <= 1,
				"pulse width timestamp interval incorrect");
		}
		expect_no_event(250);
	}
	set_enabled(1);
}

int main(int argc, char **argv)
{
	struct ptp_clock_caps caps = { 0 };
	long drift = 10000;
	int no_cross = 0;
	struct timex tx = { 0 };

	if (argc < 3 || argc > 5) {
		fprintf(stderr, "usage: %s /dev/ptpN /sys/devices/platform/virt-phc-extts/extts_schedule [freq_error_ppb] [no_crosststamp]\n", argv[0]);
		return EXIT_FAILURE;
	}
	if (argc >= 4) {
		char *end;
		errno = 0;
		drift = strtol(argv[3], &end, 10);
		if (errno || *end || drift < -62499999 || drift > 62499999) {
			fprintf(stderr, "invalid freq_error_ppb\n");
			return EXIT_FAILURE;
		}
	}
	if (argc == 5) {
		if (strcmp(argv[4], "0") && strcmp(argv[4], "1")) {
			fprintf(stderr, "no_crosststamp must be 0 or 1\n");
			return EXIT_FAILURE;
		}
		no_cross = argv[4][0] == '1';
	}
	ptp_fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (ptp_fd < 0)
		fail("open PHC");
	edge_fd = open(argv[2], O_RDWR | O_CLOEXEC);
	if (edge_fd < 0)
		fail("open edge sysfs attribute");
	atexit(cleanup);
	ptp_clock = FD_TO_CLOCKID(ptp_fd);
	if (ioctl(ptp_fd, PTP_CLOCK_GETCAPS2, &caps))
		fail("PTP_CLOCK_GETCAPS");
	require(caps.n_pins == 1 && caps.n_ext_ts == 1 && caps.n_per_out == 0 && caps.pps == 0,
		"PHC capabilities do not match one-pin, one-channel EXTTs device");
	assign_pin();
	set_enabled(1);
	check_offset_apis(no_cross);
	check_set_adjtime();

	check_schedule();
	check_polarity(drift);

	check_pair(NS_PER_SEC + drift, "intrinsic drift interval exceeds 1 ns tolerance");
	check_step_between_edges(drift);
	set_enabled(0);
	if (edge_write(raw_ns() + 200000000LL))
		fail("schedule edge while EXTTs disabled");
	expect_no_event(500);
	set_enabled(1);

	/* ADJ_FREQUENCY uses scaled ppm; cancel the configured intrinsic rate. */
	tx.modes = ADJ_FREQUENCY;
	tx.freq = (-drift * 65536L) / 1000L;
	if (clock_adjtime(ptp_clock, &tx) < 0)
		fail("clock_adjtime(PHC frequency)");
	check_pair(NS_PER_SEC, "frequency-adjusted interval differs from 1 second by over 1 ns");
	tx.modes = ADJ_FREQUENCY;
	tx.freq = 0;
	if (clock_adjtime(ptp_clock, &tx) < 0)
		fail("restore PHC frequency to zero");
	set_enabled(0);
	struct ptp_pin_desc pin = { .index = 0, .func = PTP_PF_NONE, .chan = 0 };
	if (ioctl(ptp_fd, PTP_PIN_SETFUNC2, &pin))
		fail("unassign EXTTs pin");
	set_enabled(1);
	require(edge_write(raw_ns() + 100000000LL) == 0, "schedule unassigned pin edge");
	expect_no_event(300);
	set_enabled(0);
	printf("PASS: virtual PHC EXTTs, offset APIs, edge validation, and frequency steering\n");
	return EXIT_SUCCESS;
}
