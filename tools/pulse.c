// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <linux/ptp_clock.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define NS 1000000000LL
#define SAMPLES 64
struct sample { int64_t raw, source; };
struct pulse { int64_t rise, fall; };
static volatile sig_atomic_t stopping;
static int schedule_fd = -1;

static void die(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

static void clear_schedule(void)
{
	if (schedule_fd >= 0) {
		if (write(schedule_fd, "\n", 1) != 1)
			perror("clear schedule");
		close(schedule_fd);
	}
}

static void stop(int sig)
{
	(void)sig;
	stopping = 1;
}

static int64_t now(clockid_t clock)
{
	struct timespec ts;
	if (clock_gettime(clock, &ts))
		die("clock_gettime");
	return (int64_t)ts.tv_sec * NS + ts.tv_nsec;
}

static int64_t ptp_ns(struct ptp_clock_time ts)
{
	return ts.sec * NS + ts.nsec;
}

/* The tightest bracket rejects samples delayed by scheduling or interrupts. */
static struct sample sample(int source_fd)
{
	struct sample best = {0};
	int64_t shortest = INT64_MAX;
	if (source_fd >= 0) {
		struct ptp_sys_offset_precise cross = {0};
		if (ioctl(source_fd, PTP_SYS_OFFSET_PRECISE, &cross))
			die("reference PHC requires PTP_SYS_OFFSET_PRECISE");
		return (struct sample){ptp_ns(cross.sys_monoraw), ptp_ns(cross.device)};
	}
	for (int i = 0; i < 16; i++) {
		int64_t before = now(CLOCK_MONOTONIC_RAW);
		int64_t source = now(CLOCK_REALTIME);
		int64_t after = now(CLOCK_MONOTONIC_RAW);
		if (after - before < shortest) {
			shortest = after - before;
			best = (struct sample){before + shortest / 2, source};
		}
	}
	return best;
}

/* Fit differences, not epoch timestamps, to retain nanosecond precision. */
static void fit(const struct sample *samples, int count, long double *rate,
		long double *offset)
{
	long double x = 0, y = 0, xx = 0, xy = 0;
	for (int i = 0; i < count; i++) {
		long double dx = samples[i].raw - samples[0].raw;
		long double dy = samples[i].source - samples[0].source;
		x += dx;
		y += dy;
		xx += dx * dx;
		xy += dx * dy;
	}
	*rate = (xy - x * y / count) / (xx - x * x / count);
	*offset = (y - *rate * x) / count;
}

static int64_t deadline(int64_t source, const struct sample *origin,
		long double rate, long double offset)
{
	return origin->raw + llroundl((source - origin->source - offset) / rate);
}

static void publish(const struct pulse *pulses, int count, int64_t raw)
{
	char buf[512];
	size_t used = 0;
	for (int i = 0; i < count; i++) {
		if (pulses[i].fall <= raw)
			continue;
		if (pulses[i].rise > raw)
			used += snprintf(buf + used, sizeof(buf) - used, "%" PRId64,
				pulses[i].rise);
		used += snprintf(buf + used, sizeof(buf) - used, ",%" PRId64 " ",
			pulses[i].fall);
	}
	buf[used++] = '\n';
	if (write(schedule_fd, buf, used) != (ssize_t)used)
		die("write extts_schedule");
}

static void usage(FILE *out, const char *name)
{
	fprintf(out, "Usage: %s [--schedule PATH] [--source system|/dev/ptpN]\n"
		"                  [--width SECONDS] [--count N]\n"
		"Schedule 1 Hz pulses on whole reference seconds (100 ms width by default).\n"
		"Runs until SIGINT/SIGTERM unless a positive pulse count is given.\n"
		"Owns the entire schedule and clears it on exit; does not enable capture.\n",
		name);
}

int main(int argc, char **argv)
{
	const char *path = "/sys/devices/platform/virt-phc-extts/extts_schedule";
	const char *source = "system";
	int64_t width = NS / 10, limit = 0, sent = 0, next = 0;
	struct sample samples[SAMPLES];
	struct pulse pulses[4];
	int source_fd = -1, count = 0, npulses = 0, option;
	const struct option options[] = {
		{"schedule", required_argument, NULL, 's'},
		{"source", required_argument, NULL, 'r'},
		{"width", required_argument, NULL, 'w'},
		{"count", required_argument, NULL, 'n'},
		{"help", no_argument, NULL, 'h'}, {0}
	};
	while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
		char *end;
		double value;
		switch (option) {
		case 's': path = optarg; break;
		case 'r': source = optarg; break;
		case 'w':
			errno = 0;
			value = strtod(optarg, &end);
			if (errno || end == optarg || *end || !isfinite(value) ||
			    value < 0.000001 || value >= 1)
				goto bad;
			width = llround(value * NS);
			if (width >= NS)
				goto bad;
			break;
		case 'n':
			errno = 0;
			limit = strtoll(optarg, &end, 10);
			if (errno || end == optarg || *end || limit <= 0)
				goto bad;
			break;
		case 'h': usage(stdout, argv[0]); return 0;
		default: goto bad;
		}
	}
	if (optind != argc)
		goto bad;
	if (strcmp(source, "system")) {
		source_fd = open(source, O_RDONLY);
		if (source_fd < 0)
			die("open reference PHC");
	}
	signal(SIGINT, stop);
	signal(SIGTERM, stop);
	/* Check the source before taking ownership of the schedule. */
	samples[0] = sample(source_fd);
	schedule_fd = open(path, O_WRONLY);
	if (schedule_fd < 0)
		die("open extts_schedule");
	atexit(clear_schedule);
	if (write(schedule_fd, "\n", 1) != 1)
		die("clear initial schedule");
	fprintf(stderr, "1 Hz pulses from %s, width %.9f s; fitting clock mapping\n",
		source, (double)width / NS);
	while (!stopping) {
		struct sample current = sample(source_fd);
		long double rate, offset;
		if (count && llabs((current.source - samples[count - 1].source) -
			(current.raw - samples[count - 1].raw)) > NS / 100) {
			fprintf(stderr, "Reference clock stepped; restart pulse generator\n");
			return 1;
		}
		if (count == SAMPLES) {
			memmove(samples, samples + 1, (SAMPLES - 1) * sizeof(*samples));
			count--;
		}
		samples[count++] = current;
		if (count < 10)
			goto sleep;
		fit(samples, count, &rate, &offset);
		if (!next)
			next = (current.source / NS + 2) * NS;
		while (npulses && pulses[0].fall <= current.raw) {
			memmove(pulses, pulses + 1, --npulses * sizeof(*pulses));
		}
		if (limit && sent == limit && !npulses)
			break;
		if ((!limit || sent < limit) && next < current.source + 2 * NS) {
			if (next <= current.source) {
				fprintf(stderr, "Missed pulse deadline; process was delayed too long\n");
				return 1;
			}
			pulses[npulses++] = (struct pulse){
				deadline(next, samples, rate, offset),
				deadline(next + width, samples, rate, offset)
			};
			publish(pulses, npulses, current.raw);
			next += NS;
			sent++;
		}
	sleep:;
		struct timespec delay = {.tv_nsec = 100000000};
		while (nanosleep(&delay, &delay) && errno == EINTR && !stopping) {}
	}
	fprintf(stderr, "Stopped after scheduling %" PRId64 " pulses\n", sent);
	if (source_fd >= 0)
		close(source_fd);
	return 0;
bad:
	usage(stderr, argv[0]);
	return 2;
}
