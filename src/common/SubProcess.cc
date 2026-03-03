#include "SubProcess.h"

#if defined(__FreeBSD__) || defined(__APPLE__)
#include <sys/types.h>
#include <signal.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <spawn.h>
#include <iostream>

#include "common/errno.h"
#include "include/ceph_assert.h"
#include "include/compat.h"

extern char **environ;

SubProcess::SubProcess(const char *cmd_, std_fd_op stdin_op_, std_fd_op stdout_op_, std_fd_op stderr_op_) :
  cmd(cmd_),
  cmd_args(),
  stdin_op(stdin_op_),
  stdout_op(stdout_op_),
  stderr_op(stderr_op_),
  stdin_pipe_out_fd(-1),
  stdout_pipe_in_fd(-1),
  stderr_pipe_in_fd(-1),
  pid(-1),
  errstr() {
}

SubProcess::~SubProcess() {
  ceph_assert(!is_spawned());
  ceph_assert(stdin_pipe_out_fd == -1);
  ceph_assert(stdout_pipe_in_fd == -1);
  ceph_assert(stderr_pipe_in_fd == -1);
}

void SubProcess::add_cmd_args(const char *arg, ...) {
  ceph_assert(!is_spawned());

  va_list ap;
  va_start(ap, arg);
  const char *p = arg;
  do {
    add_cmd_arg(p);
    p = va_arg(ap, const char*);
  } while (p != NULL);
  va_end(ap);
}

void SubProcess::add_cmd_arg(const char *arg) {
  ceph_assert(!is_spawned());

  cmd_args.push_back(arg);
}

int SubProcess::get_stdin() const {
  ceph_assert(is_spawned());
  ceph_assert(stdin_op == PIPE);

  return stdin_pipe_out_fd;
}

int SubProcess::get_stdout() const {
  ceph_assert(is_spawned());
  ceph_assert(stdout_op == PIPE);

  return stdout_pipe_in_fd;
}

int SubProcess::get_stderr() const {
  ceph_assert(is_spawned());
  ceph_assert(stderr_op == PIPE);

  return stderr_pipe_in_fd;
}

void SubProcess::close(int &fd) {
  if (fd == -1)
    return;

  ::close(fd);
  fd = -1;
}

void SubProcess::close_stdin() {
  ceph_assert(is_spawned());
  ceph_assert(stdin_op == PIPE);

  close(stdin_pipe_out_fd);
}

void SubProcess::close_stdout() {
  ceph_assert(is_spawned());
  ceph_assert(stdout_op == PIPE);

  close(stdout_pipe_in_fd);
}

void SubProcess::close_stderr() {
  ceph_assert(is_spawned());
  ceph_assert(stderr_op == PIPE);

  close(stderr_pipe_in_fd);
}

void SubProcess::kill(int signo) const {
  ceph_assert(is_spawned());

  int ret = ::kill(pid, signo);
  ceph_assert(ret == 0);
}

const std::string SubProcess::err() const {
  return errstr.str();
}

class fd_buf : public std::streambuf {
  int fd;
public:
  fd_buf (int fd) : fd(fd)
  {}
protected:
  int_type overflow (int_type c) override {
    if (c == EOF) return EOF;
    char buf = c;
    if (write (fd, &buf, 1) != 1) {
      return EOF;
    }
    return c;
  }
  std::streamsize xsputn (const char* s, std::streamsize count) override {
    return write(fd, s, count);
  }
};

int SubProcess::spawn() {
  ceph_assert(!is_spawned());
  ceph_assert(stdin_pipe_out_fd == -1);
  ceph_assert(stdout_pipe_in_fd == -1);
  ceph_assert(stderr_pipe_in_fd == -1);

  enum { IN = 0, OUT = 1 };

  int ipipe[2], opipe[2], epipe[2];

  ipipe[0] = ipipe[1] = opipe[0] = opipe[1] = epipe[0] = epipe[1] = -1;

  int ret = 0;

  if ((stdin_op == PIPE  && pipe_cloexec(ipipe, 0) == -1) ||
      (stdout_op == PIPE && pipe_cloexec(opipe, 0) == -1) ||
      (stderr_op == PIPE && pipe_cloexec(epipe, 0) == -1)) {
    ret = -errno;
    errstr << "pipe failed: " << cpp_strerror(errno);
    goto fail;
  }

  // Use a scope to avoid goto crossing variable initialization
  {
    // Prepare arguments for posix_spawn
    std::vector<const char *> args;
    args.push_back(cmd.c_str());
    for (std::vector<std::string>::iterator i = cmd_args.begin();
         i != cmd_args.end();
         i++) {
      args.push_back(i->c_str());
    }
    args.push_back(NULL);

    // Setup posix_spawn attributes and file actions
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t facts;
    
    ret = posix_spawnattr_init(&attr);
    if (ret != 0) {
      errstr << "posix_spawnattr_init failed: " << cpp_strerror(ret);
      ret = -ret;
      goto fail;
    }

    ret = posix_spawn_file_actions_init(&facts);
    if (ret != 0) {
      errstr << "posix_spawn_file_actions_init failed: " << cpp_strerror(ret);
      ret = -ret;
      posix_spawnattr_destroy(&attr);
      goto fail;
    }

    // Configure file actions for stdin
    if (stdin_op == PIPE) {
      if (ipipe[IN] != STDIN_FILENO) {
        posix_spawn_file_actions_adddup2(&facts, ipipe[IN], STDIN_FILENO);
      }
      posix_spawn_file_actions_addclose(&facts, ipipe[OUT]);
      posix_spawn_file_actions_addclose(&facts, ipipe[IN]);
    } else if (stdin_op == CLOSE) {
      posix_spawn_file_actions_addclose(&facts, STDIN_FILENO);
    }

    // Configure file actions for stdout
    if (stdout_op == PIPE) {
      if (opipe[OUT] != STDOUT_FILENO) {
        posix_spawn_file_actions_adddup2(&facts, opipe[OUT], STDOUT_FILENO);
      }
      posix_spawn_file_actions_addclose(&facts, opipe[IN]);
      posix_spawn_file_actions_addclose(&facts, opipe[OUT]);
    } else if (stdout_op == CLOSE) {
      posix_spawn_file_actions_addclose(&facts, STDOUT_FILENO);
    }

    // Configure file actions for stderr
    if (stderr_op == PIPE) {
      if (epipe[OUT] != STDERR_FILENO) {
        posix_spawn_file_actions_adddup2(&facts, epipe[OUT], STDERR_FILENO);
      }
      posix_spawn_file_actions_addclose(&facts, epipe[IN]);
      posix_spawn_file_actions_addclose(&facts, epipe[OUT]);
    } else if (stderr_op == CLOSE) {
      posix_spawn_file_actions_addclose(&facts, STDERR_FILENO);
    }

    // Close all other file descriptors
    int maxfd = sysconf(_SC_OPEN_MAX);
    if (maxfd == -1)
      maxfd = 16384;

#if defined(__linux__) && defined(SYS_close_range)
    // Try to use close_range if available - we'll do this in a helper
    // Note: posix_spawn doesn't have a direct way to call close_range,
    // so we need to close FDs individually or use POSIX_SPAWN_CLOEXEC_DEFAULT
    // if available (glibc 2.34+)
#endif

    // Close file descriptors above stderr (best effort with posix_spawn)
    // Note: This is a limitation - posix_spawn doesn't provide as fine-grained
    // control as fork/exec for closing all FDs. We rely on CLOEXEC flags.
    // For critical FDs, we explicitly close them.
    for (int fd = STDERR_FILENO + 1; fd < 256; fd++) {
      // Only close a reasonable range to avoid performance issues
      if (fd == ipipe[IN] || fd == ipipe[OUT] ||
          fd == opipe[IN] || fd == opipe[OUT] ||
          fd == epipe[IN] || fd == epipe[OUT])
        continue;
      posix_spawn_file_actions_addclose(&facts, fd);
    }

    // Set flags to reset signal handlers to default
    short flags = POSIX_SPAWN_SETSIGDEF;
    sigset_t defmask;
    sigemptyset(&defmask);
    posix_spawnattr_setsigdefault(&attr, &defmask);
    posix_spawnattr_setflags(&attr, flags);

    // Spawn the process
    ret = posix_spawnp(&pid, cmd.c_str(), &facts, &attr,
                       (char * const *)&args[0], environ);

    posix_spawn_file_actions_destroy(&facts);
    posix_spawnattr_destroy(&attr);

    if (ret != 0) {
      errstr << "posix_spawnp failed: " << cpp_strerror(ret);
      ret = -ret;
      goto fail;
    }
  }

  // Parent process - close child ends of pipes
  stdin_pipe_out_fd = ipipe[OUT]; close(ipipe[IN ]);
  stdout_pipe_in_fd = opipe[IN ]; close(opipe[OUT]);
  stderr_pipe_in_fd = epipe[IN ]; close(epipe[OUT]);
  return 0;

fail:
  close(ipipe[0]);
  close(ipipe[1]);
  close(opipe[0]);
  close(opipe[1]);
  close(epipe[0]);
  close(epipe[1]);

  return ret;
}

void SubProcess::exec() {
  // This function is now only called from SubProcessTimed::exec()
  // after a fork() in the timed subprocess implementation.
  // For the regular spawn() path, we use posix_spawn directly.
  ceph_assert(is_child());

  std::vector<const char *> args;
  args.push_back(cmd.c_str());
  for (std::vector<std::string>::iterator i = cmd_args.begin();
       i != cmd_args.end();
       i++) {
    args.push_back(i->c_str());
  }
  args.push_back(NULL);

  int ret = execvp(cmd.c_str(), (char * const *)&args[0]);
  ceph_assert(ret == -1);

  std::cerr << cmd << ": exec failed: " << cpp_strerror(errno) << "\n";
  _exit(EXIT_FAILURE);
}

int SubProcess::join() {
  ceph_assert(is_spawned());

  close(stdin_pipe_out_fd);
  close(stdout_pipe_in_fd);
  close(stderr_pipe_in_fd);

  int status;

  while (waitpid(pid, &status, 0) == -1)
    ceph_assert(errno == EINTR);

  pid = -1;

  if (WIFEXITED(status)) {
    if (WEXITSTATUS(status) != EXIT_SUCCESS)
      errstr << cmd << ": exit status: " << WEXITSTATUS(status);
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    errstr << cmd << ": got signal: " << WTERMSIG(status);
    return 128 + WTERMSIG(status);
  }
  errstr << cmd << ": waitpid: unknown status returned\n";
  return EXIT_FAILURE;
}

SubProcessTimed::SubProcessTimed(const char *cmd, std_fd_op stdin_op,
				 std_fd_op stdout_op, std_fd_op stderr_op,
				 int timeout_, int sigkill_) :
  SubProcess(cmd, stdin_op, stdout_op, stderr_op),
  timeout(timeout_),
  sigkill(sigkill_) {
}

static bool timedout = false; // only used after fork
void timeout_sighandler(int sig) {
  timedout = true;
}
static void dummy_sighandler(int sig) {}

void SubProcessTimed::exec() {
  ceph_assert(is_child());

  if (timeout <= 0) {
    SubProcess::exec();
    ceph_abort(); // Never reached
  }

  sigset_t mask, oldmask;
  pid_t child_pid;
  int ret;

  // Restore default action for SIGTERM in case the parent process decided
  // to ignore it.
  if (signal(SIGTERM, SIG_DFL) == SIG_ERR) {
    std::cerr << cmd << ": signal failed: " << cpp_strerror(errno) << "\n";
    goto fail_exit;
  }
  // Because SIGCHLD is ignored by default, setup dummy handler for it,
  // so we can mask it.
  if (signal(SIGCHLD, dummy_sighandler) == SIG_ERR) {
    std::cerr << cmd << ": signal failed: " << cpp_strerror(errno) << "\n";
    goto fail_exit;
  }
  // Setup timeout handler.
  if (signal(SIGALRM, timeout_sighandler) == SIG_ERR) {
    std::cerr << cmd << ": signal failed: " << cpp_strerror(errno) << "\n";
    goto fail_exit;
  }
  // Block interesting signals.
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGCHLD);
  sigaddset(&mask, SIGALRM);
  if (sigprocmask(SIG_SETMASK, &mask, &oldmask) == -1) {
    std::cerr << cmd << ": sigprocmask failed: " << cpp_strerror(errno) << "\n";
    goto fail_exit;
  }

  // Prepare arguments for posix_spawn
  {
    std::vector<const char *> args;
    args.push_back(cmd.c_str());
    for (std::vector<std::string>::iterator i = cmd_args.begin();
         i != cmd_args.end();
         i++) {
      args.push_back(i->c_str());
    }
    args.push_back(NULL);

    // Setup posix_spawn attributes and file actions
    posix_spawnattr_t attr;
    posix_spawn_file_actions_t facts;
    
    ret = posix_spawnattr_init(&attr);
    if (ret != 0) {
      std::cerr << cmd << ": posix_spawnattr_init failed: " << cpp_strerror(ret) << "\n";
      goto fail_exit;
    }

    ret = posix_spawn_file_actions_init(&facts);
    if (ret != 0) {
      std::cerr << cmd << ": posix_spawn_file_actions_init failed: " << cpp_strerror(ret) << "\n";
      posix_spawnattr_destroy(&attr);
      goto fail_exit;
    }

    // Set process group - make the child a process group leader
    short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK;
    posix_spawnattr_setpgroup(&attr, 0);  // 0 means create new process group
    posix_spawnattr_setsigmask(&attr, &oldmask);  // Restore signal mask in child
    posix_spawnattr_setflags(&attr, flags);

    // Spawn the process
    ret = posix_spawnp(&child_pid, cmd.c_str(), &facts, &attr,
                       (char * const *)&args[0], environ);

    posix_spawn_file_actions_destroy(&facts);
    posix_spawnattr_destroy(&attr);

    if (ret != 0) {
      std::cerr << cmd << ": posix_spawnp failed: " << cpp_strerror(ret) << "\n";
      goto fail_exit;
    }
  }

  // Parent (timeout monitor)
  (void)alarm(timeout);

  for (;;) {
    int signo;
    if (sigwait(&mask, &signo) == -1) {
      std::cerr << cmd << ": sigwait failed: " << cpp_strerror(errno) << "\n";
      goto fail_exit;
    }
    switch (signo) {
    case SIGCHLD:
      int status;
      if (waitpid(child_pid, &status, WNOHANG) == -1) {
	std::cerr << cmd << ": waitpid failed: " << cpp_strerror(errno) << "\n";
	goto fail_exit;
      }
      if (WIFEXITED(status))
	_exit(WEXITSTATUS(status));
      if (WIFSIGNALED(status))
	_exit(128 + WTERMSIG(status));
      std::cerr << cmd << ": unknown status returned\n";
      goto fail_exit;
    case SIGINT:
    case SIGTERM:
      // Pass SIGINT and SIGTERM, which are usually used to terminate
      // a process, to the child.
      if (::kill(child_pid, signo) == -1) {
	std::cerr << cmd << ": kill failed: " << cpp_strerror(errno) << "\n";
	goto fail_exit;
      }
      continue;
    case SIGALRM:
      std::cerr << cmd << ": timed out (" << timeout << " sec)\n";
      if (::killpg(child_pid, sigkill) == -1) {
	std::cerr << cmd << ": kill failed: " << cpp_strerror(errno) << "\n";
	goto fail_exit;
      }
      continue;
    default:
      std::cerr << cmd << ": sigwait: invalid signal: " << signo << "\n";
      goto fail_exit;
    }
  }

fail_exit:
  _exit(EXIT_FAILURE);
}
