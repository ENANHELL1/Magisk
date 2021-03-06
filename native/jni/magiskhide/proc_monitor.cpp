#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ptrace.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <vector>
#include <bitset>

#include <magisk.h>
#include <utils.h>

#include "magiskhide.h"

using namespace std;

static int inotify_fd = -1;

static void term_thread(int sig = SIGTERMTHRD);
static void new_zygote(int pid);

/**********************
 * All data structures
 **********************/

set<pair<string, string>> hide_set;                 /* set of <pkg, process> pair */
static map<int, struct stat> zygote_map;            /* zygote pid -> mnt ns */
static map<int, vector<string_view>> uid_proc_map;  /* uid -> list of process */

pthread_mutex_t monitor_lock;

#define PID_MAX 32768
static bitset<PID_MAX> attaches;  /* true if pid is monitored */
static bitset<PID_MAX> detaches;  /* true if tid should be detached */

/********
 * Utils
 ********/

static inline int read_ns(const int pid, struct stat *st) {
	char path[32];
	sprintf(path, "/proc/%d/ns/mnt", pid);
	return stat(path, st);
}

static int parse_ppid(int pid) {
	char path[32];
	int ppid;

	sprintf(path, "/proc/%d/stat", pid);
	FILE *stat = fopen(path, "re");
	if (stat == nullptr)
		return -1;

	/* PID COMM STATE PPID ..... */
	fscanf(stat, "%*d %*s %*c %d", &ppid);
	fclose(stat);

	return ppid;
}

static inline long xptrace(int request, pid_t pid, void *addr, void *data) {
	long ret = ptrace(request, pid, addr, data);
	if (ret < 0)
		PLOGE("ptrace %d", pid);
	return ret;
}

static inline long xptrace(int request, pid_t pid, void *addr = nullptr, intptr_t data = 0) {
	return xptrace(request, pid, addr, reinterpret_cast<void *>(data));
}

static bool parse_packages_xml(string_view s) {
	if (!str_starts(s, "<package "))
		return true;
	/* <package key1="value1" key2="value2"....> */
	char *start = (char *) s.data();
	start[s.length() - 1] = '\0';  /* Remove trailing '>' */
	start += 9;  /* Skip '<package ' */

	string_view pkg;
	for (char *tok = start; *tok;) {
		char *eql = strchr(tok, '=');
		if (eql == nullptr)
			break;
		*eql = '\0';  /* Terminate '=' */
		string_view key(tok, eql - tok);
		eql += 2;  /* Skip '="' */
		tok = strchr(eql, '\"');  /* Find closing '"' */
		*tok = '\0';
		string_view value(eql, tok - eql);
		tok += 2;
		if (key == "name") {
			for (auto &hide : hide_set) {
				if (hide.first == value) {
					pkg = hide.first;
					break;
				}
			}
			if (pkg.empty())
				return true;
		} else if (key == "userId" || key == "sharedUserId") {
			int uid = parse_int(value);
			for (auto &hide : hide_set) {
				if (hide.first == pkg)
					uid_proc_map[uid].emplace_back(hide.second);
			}
		}
	}
	return true;
}

void update_uid_map() {
	MutexGuard lock(monitor_lock);
	uid_proc_map.clear();
	file_readline("/data/system/packages.xml", parse_packages_xml, true);
}

static void check_zygote() {
	crawl_procfs([](int pid) -> bool {
		char buf[512];
		snprintf(buf, sizeof(buf), "/proc/%d/cmdline", pid);
		if (FILE *f = fopen(buf, "re"); f) {
			fgets(buf, sizeof(buf), f);
			if (strncmp(buf, "zygote", 6) == 0 && parse_ppid(pid) == 1)
				new_zygote(pid);
			fclose(f);
		}
		return true;
	});
}

#define APP_PROC "/system/bin/app_process"

static void setup_inotify() {
	inotify_fd = xinotify_init1(IN_CLOEXEC);
	if (inotify_fd < 0)
		term_thread();

	// Setup inotify asynchronous I/O
	fcntl(inotify_fd, F_SETFL, O_ASYNC);
	struct f_owner_ex ex = {
		.type = F_OWNER_TID,
		.pid = gettid()
	};
	fcntl(inotify_fd, F_SETOWN_EX, &ex);

	// Monitor packages.xml
	inotify_add_watch(inotify_fd, "/data/system", IN_CLOSE_WRITE);

	// Monitor app_process
	if (access(APP_PROC "32", F_OK) == 0) {
		inotify_add_watch(inotify_fd, APP_PROC "32", IN_ACCESS);
		if (access(APP_PROC "64", F_OK) == 0)
			inotify_add_watch(inotify_fd, APP_PROC "64", IN_ACCESS);
	} else {
		inotify_add_watch(inotify_fd, APP_PROC, IN_ACCESS);
	}
}

/************************
 * Async signal handlers
 ************************/

static void inotify_event(int) {
	/* Make sure we can actually read stuffs
	 * or else the whole thread will be blocked.*/
	struct pollfd pfd = {
		.fd = inotify_fd,
		.events = POLLIN,
		.revents = 0
	};
	if (poll(&pfd, 1, 0) <= 0)
		return;  // Nothing to read
	char buf[512];
	auto event = reinterpret_cast<struct inotify_event *>(buf);
	read(inotify_fd, buf, sizeof(buf));
	if ((event->mask & IN_CLOSE_WRITE) && event->name == "packages.xml"sv)
		update_uid_map();
	check_zygote();
}

// Workaround for the lack of pthread_cancel
static void term_thread(int) {
	LOGD("proc_monitor: cleaning up\n");
	uid_proc_map.clear();
	zygote_map.clear();
	hide_set.clear();
	attaches.reset();
	detaches.reset();
	// Misc
	hide_enabled = false;
	pthread_mutex_destroy(&monitor_lock);
	close(inotify_fd);
	inotify_fd = -1;
	LOGD("proc_monitor: terminate\n");
	pthread_exit(nullptr);
}

/******************
 * Ptrace Madness
 ******************/

/* Ptrace is super tricky, preserve all excessive logging in code
 * but disable when actually building for usage (you won't want
 * your logcat spammed with new thread events from all apps) */

//#define PTRACE_LOG(fmt, args...) LOGD("PID=[%d] " fmt, pid, ##args)
#define PTRACE_LOG(...)

static void detach_pid(int pid, int signal = 0) {
	char path[128];
	attaches[pid] = false;
	xptrace(PTRACE_DETACH, pid, nullptr, signal);

	// Detach all child threads too
	sprintf(path, "/proc/%d/task", pid);
	DIR *dir = opendir(path);
	crawl_procfs(dir, [&](int tid) -> bool {
		if (tid != pid) {
			// Check if we should force a SIGSTOP
			if (waitpid(tid, nullptr, __WALL | __WNOTHREAD | WNOHANG) == tid) {
				PTRACE_LOG("detach thread [%d]\n", tid);
				xptrace(PTRACE_DETACH, tid);
			} else {
				detaches[tid] = true;
				tgkill(pid, tid, SIGSTOP);
			}
		}
		return true;
	});
	closedir(dir);
}

static bool check_pid(int pid) {
	char path[128];
	char cmdline[1024];
	sprintf(path, "/proc/%d/cmdline", pid);
	FILE *f = fopen(path, "re");
	// Process killed unexpectedly, ignore
	if (!f) return true;
	fgets(cmdline, sizeof(cmdline), f);
	fclose(f);
	if (strncmp(cmdline, "zygote", 6) == 0)
		return false;

	sprintf(path, "/proc/%d", pid);
	struct stat st;
	lstat(path, &st);
	int uid = st.st_uid % 100000;
	auto it = uid_proc_map.find(uid);
	if (it != uid_proc_map.end()) {
		for (auto &s : it->second) {
			if (s == cmdline) {
				// Double check whether ns is separated
				read_ns(pid, &st);
				bool mnt_ns = true;
				for (auto &zit : zygote_map) {
					if (zit.second.st_ino == st.st_ino &&
						zit.second.st_dev == st.st_dev) {
						mnt_ns = false;
						break;
					}
				}
				// For some reason ns is not separated, abort
				if (!mnt_ns)
					break;

				/* Finally this is our target!
				 * Detach from ptrace but should still remain stopped.
				 * The hide daemon will resume the process. */
				PTRACE_LOG("target found\n");
				LOGI("proc_monitor: [%s] PID=[%d] UID=[%d]\n", cmdline, pid, uid);
				detach_pid(pid, SIGSTOP);
				if (fork_dont_care() == 0)
					hide_daemon(pid);
				return true;
			}
		}
	}
	PTRACE_LOG("[%s] not our target\n", cmdline);
	detach_pid(pid);
	return true;
}

static void new_zygote(int pid) {
	struct stat st;
	if (read_ns(pid, &st))
		return;

	auto it = zygote_map.find(pid);
	if (it != zygote_map.end()) {
		// Update namespace info
		it->second = st;
		return;
	}

	LOGD("proc_monitor: ptrace zygote PID=[%d]\n", pid);
	zygote_map[pid] = st;

	xptrace(PTRACE_ATTACH, pid);

	waitpid(pid, nullptr, __WALL | __WNOTHREAD);
	xptrace(PTRACE_SETOPTIONS, pid, nullptr,
			PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACEEXIT);
	xptrace(PTRACE_CONT, pid);
}

#define WEVENT(s) (((s) & 0xffff0000) >> 16)
#define DETACH_AND_CONT { detach = true; continue; }

void proc_monitor() {
	// Unblock some signals
	sigset_t block_set;
	sigemptyset(&block_set);
	sigaddset(&block_set, SIGTERMTHRD);
	sigaddset(&block_set, SIGIO);
	pthread_sigmask(SIG_UNBLOCK, &block_set, nullptr);

	struct sigaction act{};
	act.sa_handler = term_thread;
	sigaction(SIGTERMTHRD, &act, nullptr);
	act.sa_handler = inotify_event;
	sigaction(SIGIO, &act, nullptr);

	setup_inotify();

	// First find existing zygotes
	check_zygote();

	int status;

	for (;;) {
		const int pid = waitpid(-1, &status, __WALL | __WNOTHREAD);
		if (pid < 0) {
			if (errno == ECHILD) {
				/* This mean we have nothing to wait, sleep
				 * and wait till signal interruption */
				LOGD("proc_monitor: nothing to monitor, wait for signal\n");
				struct timespec ts = {
					.tv_sec = INT_MAX,
					.tv_nsec = 0
				};
				nanosleep(&ts, nullptr);
			}
			continue;
		}
		bool detach = false;
		RunFinally detach_task([&]() -> void {
			if (detach) {
				// Non of our business now
				attaches[pid] = false;
				detaches[pid] = false;
				ptrace(PTRACE_DETACH, pid, 0, 0);
				PTRACE_LOG("detach\n");
			}
		});

		if (!WIFSTOPPED(status) /* Ignore if not ptrace-stop */ || detaches[pid])
			DETACH_AND_CONT;

		if (WSTOPSIG(status) == SIGTRAP && WEVENT(status)) {
			unsigned long msg;
			xptrace(PTRACE_GETEVENTMSG, pid, nullptr, &msg);
			if (zygote_map.count(pid)) {
				// Zygote event
				switch (WEVENT(status)) {
					case PTRACE_EVENT_FORK:
					case PTRACE_EVENT_VFORK:
						PTRACE_LOG("zygote forked: [%d]\n", msg);
						attaches[msg] = true;
						break;
					case PTRACE_EVENT_EXIT:
						PTRACE_LOG("zygote exited with status: [%d]\n", msg);
						[[fallthrough]];
					default:
						zygote_map.erase(pid);
						DETACH_AND_CONT;
				}
			} else {
				switch (WEVENT(status)) {
					case PTRACE_EVENT_CLONE:
						PTRACE_LOG("create new threads: [%d]\n", msg);
						if (attaches[pid] && check_pid(pid))
							continue;
						break;
					case PTRACE_EVENT_EXEC:
					case PTRACE_EVENT_EXIT:
						PTRACE_LOG("exit or execve\n");
						[[fallthrough]];
					default:
						DETACH_AND_CONT;
				}
			}
			xptrace(PTRACE_CONT, pid);
		} else if (WSTOPSIG(status) == SIGSTOP) {
			PTRACE_LOG("SIGSTOP from child\n");
			xptrace(PTRACE_SETOPTIONS, pid, nullptr,
					PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXEC | PTRACE_O_TRACEEXIT);
			xptrace(PTRACE_CONT, pid);
		} else {
			// Not caused by us, resend signal
			xptrace(PTRACE_CONT, pid, nullptr, WSTOPSIG(status));
			PTRACE_LOG("signal [%d]\n", WSTOPSIG(status));
		}
	}
}
