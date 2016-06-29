#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include "SystemRecorder.hpp"
#include "SystemMonitor.hpp"

#include <unistd.h>

#define SIZEOF_ARRAY(array) (sizeof(array)/sizeof(array[0]))

static struct Context {
	bool stop = false;
	int stopfd = -1;
	int timer = -1;
} ctx;

struct Params {
	bool help = false;
	std::string output;
	int period = 1;
};

static const struct option argsOptions[] = {
	{ "help"  , optional_argument, 0, 'h' },
	{ "period", optional_argument, 0, 'p' },
	{ "output", required_argument, 0, 'o' },
	{ 0, 0, 0}
};

int parseArgs(int argc, char *argv[], Params *params)
{
	int optionIndex = 0;
	int value;

	while (true) {
		value = getopt_long(argc, argv, "ho:p:", argsOptions, &optionIndex);
		if (value == -1 || value == '?')
			break;

		switch (value) {
		case 'h':
			params->help = true;
			break;

		case 'o':
			params->output = optarg;
			break;

		case 'p': {
			char *end;
			long int v;

			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno != 0) {
				int ret = -errno;
				printf("Unable to parse period %s : %d(%m)\n",
				       optarg, errno);
				return ret;
			} else if (*end != '\0') {
				printf("Period arg '%s' is not decimal\n",
				        optarg);
				return -EINVAL;
			} else if (v <= 0) {
				printf("Period arg '%s' is negative or null\n",
				        optarg);
				return -EINVAL;
			}

			params->period = v;
			break;
		}

		default:
			break;
		}
	}

	return 0;
}

void printUsage(int argc, char *argv[])
{
	printf("Usage  %s [-h] [-p PERIOD] -o OUTPUT process [process ...]\n",
	       argv[0]);

	printf("\n");

	printf("positional arguments:\n");
	printf("  %-20s %s\n", "process", "Process name to monitor");

	printf("\n");

	printf("optional arguments:\n");
	printf("  %-20s %s\n", "-h, --help", "show this help message and exit");
	printf("  %-20s %s\n", "-p, --period", "sample acquisition period (seconds). Default : 1");
	printf("  %-20s %s\n", "-o, --output", "output record file");
}

static void sighandler(int s)
{
	int64_t stop = 1;
	int ret;

	printf("stop\n");
	ctx.stop = true;
	ret = write(ctx.stopfd, &stop, sizeof(stop));
	if (ret < 0)
		printf("write() failed : %d(%m)\n", errno);
}

int main(int argc, char *argv[])
{
	struct itimerspec timer;
	struct pollfd fds[2];
	Params params;
	SystemMonitor::Callbacks cb;
	SystemMonitor *mon = nullptr;
	SystemRecorder *recorder = nullptr;
	int ret;

	signal(SIGINT, sighandler);

	// Parse parameters
	if (argc == 1) {
		printUsage(argc, argv);
		return 0;
	}

	ret = parseArgs(argc, argv, &params);
	if (ret < 0) {
		printUsage(argc, argv);
		return 1;
	} else if (params.output.empty()) {
		printf("No output file specified\n\n");
		printUsage(argc, argv);
		return 1;
	} else if (optind == argc) {
		printf("No process name specified\n\n");
		printUsage(argc, argv);
		return 1;
	}

	// Create recorder
	recorder = SystemRecorder::create();
	if (!recorder)
		goto error;

	ret = recorder->open(params.output.c_str());
	if (ret < 0) {
		printf("open() failed : %d(%s)\n", -ret, strerror(-ret));
	}

	// Create monitor
	cb.mProcessStats = [recorder] (const SystemMonitor::ProcessStats &stats) {
		int ret;

		ret = recorder->record(stats);
		if (ret < 0)
			printf("record() failed : %d(%s)\n", -ret, strerror(-ret));
	};

	cb.mThreadStats = [recorder] (const SystemMonitor::ThreadStats &stats) {
		int ret;

		ret = recorder->record(stats);
		if (ret < 0)
			printf("record() failed : %d(%s)\n", -ret, strerror(-ret));
	};

	cb.mAcquisitionDuration = [recorder] (const SystemMonitor::AcquisitionDuration &duration) {
		int ret;

		ret = recorder->record(duration);
		if (ret < 0)
			printf("recordDuration() failed : %d(%s)\n",
			       -ret, strerror(-ret));
	};

	mon = SystemMonitor::create(cb);
	if (!mon)
		goto error;

	for (int i = optind; i < argc; i++) {
		ret = mon->addProcess(argv[i]);
		if (ret < 0) {
			printf("addProcessFailed() : %d(%s)\n", -ret, strerror(-ret));
			goto error;
		}

	}

	// Create stopfd
	ret = eventfd(0, EFD_CLOEXEC);
	if (ret < 0) {
		printf("eventfd() failed : %d(%m)\n", errno);
		goto error;
	}

	ctx.stopfd = ret;

	// Create timer
	ret = timerfd_create(CLOCK_MONOTONIC, EFD_CLOEXEC);
	if (ret < 0) {
		printf("timerfd_create() failed : %d(%m)\n", errno);
		goto error;
	}

	ctx.timer = ret;

	// Start timer
	timer.it_interval.tv_sec = params.period;
	timer.it_interval.tv_nsec = 0;

	timer.it_value.tv_sec = params.period;
	timer.it_value.tv_nsec = 0;

	ret = timerfd_settime(ctx.timer, 0, &timer, NULL);
	if (ret < 0) {
		printf("timerfd_settime() failed : %d(%m)\n", errno);
		goto error;
	}

	// Start poll
	fds[0].fd = ctx.stopfd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	fds[1].fd = ctx.timer;
	fds[1].events = POLLIN;
	fds[1].revents = 0;

	while (true) {
		do {
			ret = poll(fds, SIZEOF_ARRAY(fds), -1);
		} while (ret == -1 && errno == EINTR);

		if (ret == -1) {
			printf("poll() failed : %d(%m)\n", errno);
			goto error;
		} else if (ret == 0) {
			printf("poll() : timeout\n");
			continue;
		}

		if (ctx.stop)
			break;

		if (fds[1].events & POLLIN) {
			uint64_t expirations;

			ret = read(fds[1].fd, &expirations, sizeof(expirations));
			if (ret < 0)
				printf("read() failed : %d(%m)\n", errno);

			mon->process();
		}
	}

	recorder->close();

	delete mon;
	delete recorder;

	if (ctx.stopfd != -1)
		close(ctx.stopfd);

	if (ctx.timer != -1)
		close(ctx.timer);

	return 0;

error:
	delete mon;
	delete recorder;

	if (ctx.stopfd != -1)
		close(ctx.stopfd);

	if (ctx.timer != -1)
		close(ctx.timer);

	return 1;
}
