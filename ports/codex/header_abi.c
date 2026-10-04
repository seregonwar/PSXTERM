/* Compile-only SDK header inventory. No entry point or executable behavior. */
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <pthread.h>
#include <signal.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>

#define VALUE(name, expression) \
    const uint64_t psx_abi_##name = (uint64_t)(expression)
#define SIZE(name, type) VALUE(name##_size, sizeof(type))
#define ALIGN(name, type) VALUE(name##_align, _Alignof(type))
#define FIELD(name, type, member) VALUE(name##_##member##_offset, offsetof(type, member))

SIZE(pointer, void *);
SIZE(time_t, time_t);
SIZE(off_t, off_t);
SIZE(ino_t, ino_t);
SIZE(nlink_t, nlink_t);
SIZE(pthread_t, pthread_t);
SIZE(pthread_mutex_t, pthread_mutex_t);
SIZE(pthread_cond_t, pthread_cond_t);
SIZE(pthread_rwlock_t, pthread_rwlock_t);
SIZE(pthread_attr_t, pthread_attr_t);
SIZE(sigset_t, sigset_t);
SIZE(stat, struct stat);
ALIGN(stat, struct stat);
FIELD(stat, struct stat, st_ino);
FIELD(stat, struct stat, st_nlink);
FIELD(stat, struct stat, st_size);
FIELD(stat, struct stat, st_atim);
FIELD(stat, struct stat, st_mtim);
SIZE(kevent, struct kevent);
ALIGN(kevent, struct kevent);
FIELD(kevent, struct kevent, ident);
FIELD(kevent, struct kevent, flags);
FIELD(kevent, struct kevent, data);
FIELD(kevent, struct kevent, udata);
SIZE(timespec, struct timespec);
FIELD(timespec, struct timespec, tv_nsec);
SIZE(sockaddr, struct sockaddr);
FIELD(sockaddr, struct sockaddr, sa_family);
SIZE(sockaddr_storage, struct sockaddr_storage);
SIZE(termios, struct termios);
FIELD(termios, struct termios, c_cc);
VALUE(evfilt_read, EVFILT_READ);
VALUE(ev_add, EV_ADD);
VALUE(af_inet, AF_INET);
VALUE(af_inet6, AF_INET6);
VALUE(clock_monotonic, CLOCK_MONOTONIC);
