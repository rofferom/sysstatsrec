#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#include <string>
#include <list>
#include <map>

#include "SystemMonitor.hpp"

namespace {

#define INVALID_PID -1

#define STAT_PATTERN \
	"%d %128s %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld "\
	"%ld %ld %llu %lu %ld"

#define STAT_PATTERN_COUNT 24

struct RawStats {
	int pid;
	char name[128];
	char state;
	int ppid;
	int pgrp;
	int session;
	int tty_nr;
	int tpgid;
	unsigned int flags;
	unsigned long int minflt;
	unsigned long int cminflt;
	unsigned long int majflt;
	unsigned long int cmajflt;
	unsigned long int utime;
	unsigned long int stime;
	long int cutime;
	long int cstime;
	long int priority;
	long int nice;
	long int num_threads;
	long int itrealvalue;
	unsigned long long int starttime;
	unsigned long int vsize;
	long int rss;

	RawStats()
	{
		pid = 0;
		name[0] = '\0';
		state = 0;
		ppid = 0;
		pgrp = 0;
		session = 0;
		tty_nr = 0;
		tpgid = 0;
		flags = 0;
		minflt = 0;
		cminflt = 0;
		majflt = 0;
		cmajflt = 0;
		utime = 0;
		stime = 0;
		cutime = 0;
		cstime = 0;
		priority = 0;
		nice = 0;
		num_threads = 0;
		itrealvalue = 0;
		starttime = 0;
		vsize = 0;
		rss = 0;
	}
};

struct SystemSettings {
	int mHertz;
	int mPagesize ;
};

class ProcessMonitor {
private:
	struct ThreadInfo {
		char mPath[128];
		char mName[64];
		RawStats mPrevStats;
	};

private:
	int mPid;
	std::string mName;
	RawStats mPrevStats;
	const SystemSettings *mSysSettings;

	bool mFirstProcess;

	std::map<int, ThreadInfo> mThreads;

private:
	void clear();

	int getPidFdCount();

	static int readStats(const char *path, RawStats *procstat);
	static bool testPidName(int pid, const char *name);
	static int findProcess(const char *name, int *outPid);

	static uint16_t getCpuLoad(const RawStats &prevStats,
				   const RawStats &curStats,
				   const SystemSettings *sysSettings,
				   int timeDiff);

	int processThread(int tid,
			  uint64_t ts,
			  int timeDiff,
			  const SystemMonitor::Callbacks &cb);


	int processThreads(uint64_t ts,
			   int timeDiff,
			   const SystemMonitor::Callbacks &cb);

public:
	ProcessMonitor(const char *name, const SystemSettings *sysSettings);

	int process(uint64_t ts,
		    int timeDiff,
		    const SystemMonitor::Callbacks &cb);
};

ProcessMonitor::ProcessMonitor(const char *name,
			       const SystemSettings *sysSettings)
{
	mName = name;
	mPid = INVALID_PID;
	mSysSettings = sysSettings;
	mFirstProcess = true;
}

void ProcessMonitor::clear()
{
	mPid = INVALID_PID;
	mFirstProcess = true;
}

uint16_t ProcessMonitor::getCpuLoad(const RawStats &prevStats,
				    const RawStats &curStats,
				    const SystemSettings *sysSettings,
				    int timeDiff)
{
	long int spentTime;

	spentTime  = curStats.utime + curStats.stime;
	spentTime -= prevStats.utime + prevStats.stime;

	return (100 * spentTime) / (sysSettings->mHertz * timeDiff);
}

int ProcessMonitor::processThread(int tid,
				  uint64_t ts,
				  int timeDiff,
				  const SystemMonitor::Callbacks &cb)
{
	ThreadInfo *info;
	SystemMonitor::ThreadStats stats;
	RawStats rawStats;
	int ret;

	auto thread = mThreads.find(tid);
	if (thread == mThreads.end()) {
		ThreadInfo info;

		// Fill thread info
		snprintf(info.mPath, sizeof(info.mPath),
			 "/proc/%d/task/%d/stat",
			 mPid, tid);

		ret = readStats(info.mPath, &info.mPrevStats);
		if (ret < 0)
			return ret;

		snprintf(info.mName, sizeof(info.mName),
			 "%d-%s",
			 tid,
			 info.mPrevStats.name);

		// Register thread
		auto insertRet = mThreads.insert( {tid, info} );
		if (!insertRet.second) {
			printf("Fail to insert thread %d\n", tid);
			return -EPERM;
		}

		return -EAGAIN;
	}

	info = &thread->second;

	ret = readStats(info->mPath, &rawStats);
	if (ret < 0) {
		// Thread finished
		mThreads.erase(thread);
		return ret;
	}

	stats.mTs = ts;
	stats.mPid = mPid;
	stats.mTid = tid;
	stats.mName = info->mName;

	stats.mCpuLoad = getCpuLoad(info->mPrevStats,
				    rawStats,
				    mSysSettings,
				    timeDiff);

	info->mPrevStats = rawStats;

	if (cb.mThreadStats)
		cb.mThreadStats(stats);

	return 0;
}

int ProcessMonitor::processThreads(uint64_t ts,
				   int timeDiff,
				   const SystemMonitor::Callbacks &cb)
{
	DIR *d;
	struct dirent entry;
	struct dirent *result = nullptr;
	char path[128];
	int tid;
	char *endptr;
	int ret;

	snprintf(path, sizeof(path), "/proc/%d/task", mPid);

	d = opendir(path);
	if (!d) {
		ret = -errno;
		printf("Fail to open /proc : %d(%m)\n", errno);
		return ret;
	}

	while (true) {
		ret = readdir_r(d, &entry, &result);
		if (ret > 0) {
			printf("readdir_r() failed : %d(%s)\n",
			       ret, strerror(ret));
			ret = -ret;
			goto closedir;
		} else if (ret == 0 && !result) {
			break;
		}

		tid = strtol(entry.d_name, &endptr, 10);
		if (errno == ERANGE) {
			printf("Ignore %s\n", entry.d_name);
			continue;
		} else if (*endptr != '\0') {
			continue;
		}

		ret = processThread(tid, ts, timeDiff, cb);
		if (ret < 0 && ret != -EAGAIN)
			printf("Fail to process thread %d\n", tid);
	}

	closedir(d);

	return 0;

closedir:
	return ret;
}

int ProcessMonitor::process(uint64_t ts,
			    int timeDiff,
			    const SystemMonitor::Callbacks &cb)
{
	SystemMonitor::ProcessStats stats;
	RawStats rawStats;
	char path[64];
	int ret;

	if (mPid == INVALID_PID) {
		ret = findProcess(mName.c_str(), &mPid);
		if (ret < 0)
			return ret;
	}

	snprintf(path, sizeof(path), "/proc/%d/stat", mPid);

	if (mFirstProcess) {
		ret = readStats(path, &mPrevStats);
		if (ret < 0) {
			printf("Can't find pid stats '%s'\n", mName.c_str());
			return ret;
		}

		mFirstProcess = false;

		return -EAGAIN;
	}

	// Get stats
	ret = readStats(path, &rawStats);
	if (ret < 0) {
		printf("Can't find pid stats '%s'\n", mName.c_str());
		clear();
		return ret;
	}

	stats.mTs = ts;
	stats.mPid = mPid;
	stats.mName = mName.c_str();

	stats.mCpuLoad = getCpuLoad(mPrevStats,
				    rawStats,
				    mSysSettings,
				    timeDiff);

	stats.mVsize = rawStats.vsize / 1024;
	stats.mRss = rawStats.rss * mSysSettings->mPagesize / 1024;
	stats.mThreadCount = rawStats.num_threads;
	stats.mFdCount = getPidFdCount();

	mPrevStats = rawStats;

	if (cb.mProcessStats)
		cb.mProcessStats(stats);

	// Process threads
	processThreads(ts, timeDiff, cb);

	return 0;
}

int ProcessMonitor::getPidFdCount()
{
	DIR *d;
	struct dirent entry;
	struct dirent *result = nullptr;
	char path[64];
	int ret;
	bool found = false;
	int count = 0;

	snprintf(path, sizeof(path), "/proc/%d/fd", mPid);

	d = opendir(path);
	if (!d) {
		ret = -errno;
		printf("Fail to open /proc : %d(%m)\n", errno);
		return ret;
	}

	while (!found) {
		ret = readdir_r(d, &entry, &result);
		if (ret == 0 && !result)
			break;

		count++;
	}

	closedir(d);

	return count;
}

int ProcessMonitor::readStats(const char *path, RawStats *procstat)
{
	char strstat[1024];
	int fd;
	ssize_t readRet;
	int ret;

	// Read process stats
	fd = open(path, O_RDONLY|O_CLOEXEC);
	if (fd == -1) {
		ret = -errno;
		printf("Fail to open %s : %d(%m)\n", path, errno);
		return ret;
	}

	readRet = read(fd, strstat, sizeof(strstat));
	if (readRet == -1) {
		ret = -errno;
		printf("Fail to read %s : %d(%m)", path, errno);
		goto close_fd;
	}

	close(fd);

	// Remove trailing '\n'
	strstat[readRet - 1] = '\0';

	// Extract process name
	ret = sscanf(strstat, STAT_PATTERN,
		     &procstat->pid,
		     procstat->name,
		     &procstat->state,
		     &procstat->ppid,
		     &procstat->pgrp,
		     &procstat->session,
		     &procstat->tty_nr,
		     &procstat->tpgid,
		     &procstat->flags,
		     &procstat->minflt,
		     &procstat->cminflt,
		     &procstat->majflt,
		     &procstat->cmajflt,
		     &procstat->utime,
		     &procstat->stime,
		     &procstat->cutime,
		     &procstat->cstime,
		     &procstat->priority,
		     &procstat->nice,
		     &procstat->num_threads,
		     &procstat->itrealvalue,
		     &procstat->starttime,
		     &procstat->vsize,
		     &procstat->rss);
	if (ret != STAT_PATTERN_COUNT) {
		ret = -EINVAL;
		printf("Fail to parse path %s\n", path);
		goto error;
	}

	return 0;

close_fd:
	close(fd);
error:
	return ret;

}

bool ProcessMonitor::testPidName(int pid, const char *name)
{
	RawStats procstat;
	size_t procNameLen;
	char path[128];
	int ret;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);

	ret = readStats(path, &procstat);
	if (ret < 0)
		return false;

	procNameLen = strlen(procstat.name);
	if (procNameLen == 0) {
		printf("Got an empty process name for pid %d\n", pid);
		return false;
	}

	// Compare but exclude first and last caracter.
	// procfs give name like '(<processname>)'
	return (strncmp(name, procstat.name + 1, procNameLen - 2)) == 0;
}

int ProcessMonitor::findProcess(const char *name, int *outPid)
{
	DIR *d;
	struct dirent entry;
	struct dirent *result = nullptr;
	int pid;
	char *endptr;
	int ret;
	bool found = false;

	d = opendir("/proc");
	if (!d) {
		ret = -errno;
		printf("Fail to open /proc : %d(%m)\n", errno);
		return ret;
	}

	while (!found) {
		ret = readdir_r(d, &entry, &result);
		if (ret == 0 && !result)
			break;

		pid = strtol(entry.d_name, &endptr, 10);
		if (errno == ERANGE) {
			printf("Ignore %s\n", entry.d_name);
			continue;
		} else if (*endptr != '\0') {
			continue;
		}

		found = testPidName(pid, name);
	}

	closedir(d);

	if (found)
		*outPid = pid;

	return found ? 0 : -ENOENT;
}

class SystemMonitorImpl : public SystemMonitor {
private:
	Callbacks mCb;
	SystemSettings mSysSettings;
	std::list<ProcessMonitor *> mMonitors;
	struct timespec mLastProcess;

public:
	SystemMonitorImpl(const Callbacks &cb);
	virtual ~SystemMonitorImpl();

	virtual int addProcess(const char *name) override;
	virtual int process() override;
};

SystemMonitorImpl::SystemMonitorImpl(const Callbacks &cb) : SystemMonitor()
{
	mCb = cb;
	mSysSettings.mHertz = sysconf(_SC_CLK_TCK);
	mSysSettings.mPagesize = getpagesize();
	mLastProcess.tv_sec = 0;
	mLastProcess.tv_nsec = 0;
}

SystemMonitorImpl::~SystemMonitorImpl()
{
	for (auto &m :mMonitors)
		delete m;
}

int SystemMonitorImpl::addProcess(const char *name)
{
	ProcessMonitor *monitor;

	if (!name)
		return -EINVAL;

	monitor = new ProcessMonitor(name, &mSysSettings);
	if (!monitor)
		return -ENOMEM;

	mMonitors.push_back(monitor);

	return 0;
}

static int timespecDiff(struct timespec *start, struct timespec *stop)
{
	return stop->tv_sec - start->tv_sec;
}

int SystemMonitorImpl::process()
{
	struct timespec now;
	int timeDiff; // seconds
	int ret;

	// Compute delay between two calls
	ret = clock_gettime(CLOCK_MONOTONIC, &now);
	if (ret < 0) {
		ret = -errno;
		printf("clock_gettime() failed : %d(%m)", errno);
		return ret;
	}

	timeDiff = timespecDiff(&mLastProcess, &now);
	if (timeDiff == 0) {
		printf("timespecDiff() returned zero\n");
		return -EINVAL;
	}

	// Start process monitors
	for (auto &m :mMonitors)
		m->process(now.tv_sec, timeDiff, mCb);

	mLastProcess = now;

	return 0;
}

} // anonymous namespace

SystemMonitor *SystemMonitor::create(const Callbacks &cb)
{
	return new SystemMonitorImpl(cb);
}
