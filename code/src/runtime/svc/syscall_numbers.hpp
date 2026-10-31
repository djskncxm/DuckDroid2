// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstdint>

// Android arm64 uses the generic syscall table (asm-generic/unistd.h).
// Only the numbers M1 actually handles are listed here.
namespace duckemu::runtime::svc
{

inline constexpr uint32_t kSysGetcwd = 17;
inline constexpr uint32_t kSysEventfd2 = 19;
inline constexpr uint32_t kSysEpollCreate1 = 20;
inline constexpr uint32_t kSysEpollCtl = 21;
inline constexpr uint32_t kSysEpollPwait = 22;
inline constexpr uint32_t kSysDup = 23;
inline constexpr uint32_t kSysDup3 = 24;
inline constexpr uint32_t kSysFcntl = 25;
inline constexpr uint32_t kSysIoctl = 29;
inline constexpr uint32_t kSysFaccessat = 48;
inline constexpr uint32_t kSysOpenat = 56;
inline constexpr uint32_t kSysMkdirat = 34;
inline constexpr uint32_t kSysUnlinkat = 35;
inline constexpr uint32_t kSysRenameat = 38;
inline constexpr uint32_t kSysStatfs = 43;
inline constexpr uint32_t kSysFstatfs = 44;
inline constexpr uint32_t kSysFtruncate = 46;
inline constexpr uint32_t kSysClose = 57;
inline constexpr uint32_t kSysPipe2 = 59;
inline constexpr uint32_t kSysFsync = 82;
inline constexpr uint32_t kSysFdatasync = 83;
inline constexpr uint32_t kSysTimerfdCreate = 85;
inline constexpr uint32_t kSysTimerfdSettime = 86;
inline constexpr uint32_t kSysTimerfdGettime = 87;
inline constexpr uint32_t kSysLseek = 62;
inline constexpr uint32_t kSysRead = 63;
inline constexpr uint32_t kSysWrite = 64;
inline constexpr uint32_t kSysPread64 = 67;
inline constexpr uint32_t kSysPselect6 = 72;
inline constexpr uint32_t kSysPpoll = 73;
inline constexpr uint32_t kSysReadlinkat = 78;
inline constexpr uint32_t kSysNewfstatat = 79;
inline constexpr uint32_t kSysFstat = 80;
inline constexpr uint32_t kSysClockGettime = 113;
inline constexpr uint32_t kSysSchedSetaffinity = 122;
inline constexpr uint32_t kSysSchedGetaffinity = 123;
inline constexpr uint32_t kSysSchedYield = 124;
inline constexpr uint32_t kSysSigaltstack = 132;
inline constexpr uint32_t kSysRtSigaction = 134;
inline constexpr uint32_t kSysRtSigprocmask = 135;
inline constexpr uint32_t kSysUname = 160;
inline constexpr uint32_t kSysGetrlimit = 163;
inline constexpr uint32_t kSysGettimeofday = 169;
inline constexpr uint32_t kSysGetppid = 173;
inline constexpr uint32_t kSysGetuid = 174;
inline constexpr uint32_t kSysGeteuid = 175;
inline constexpr uint32_t kSysGetgid = 176;
inline constexpr uint32_t kSysGetegid = 177;
inline constexpr uint32_t kSysPrctl = 167;
inline constexpr uint32_t kSysPrlimit64 = 261;
inline constexpr uint32_t kSysSetRobustList = 273;
inline constexpr uint32_t kSysGetrandom = 278;
inline constexpr uint32_t kSysMembarrier = 283;
inline constexpr uint32_t kSysFaccessat2 = 439;
inline constexpr uint32_t kSysExit = 93;
inline constexpr uint32_t kSysExitGroup = 94;
inline constexpr uint32_t kSysSetTidAddress = 96;
inline constexpr uint32_t kSysFutex = 98;
inline constexpr uint32_t kSysNanosleep = 101;
inline constexpr uint32_t kSysClockNanosleep = 115;
inline constexpr uint32_t kSysKill = 129;
inline constexpr uint32_t kSysTkill = 130;
inline constexpr uint32_t kSysTgkill = 131;
inline constexpr uint32_t kSysGetpid = 172;
inline constexpr uint32_t kSysGettid = 178;
inline constexpr uint32_t kSysBrk = 214;
inline constexpr uint32_t kSysMunmap = 215;
inline constexpr uint32_t kSysClone = 220;
inline constexpr uint32_t kSysMmap = 222;
inline constexpr uint32_t kSysMprotect = 226;
inline constexpr uint32_t kSysMadvise = 233;

// Linux/bionic errno values (identical on host and guest).
inline constexpr int64_t kEperm = 1;
inline constexpr int64_t kEnoent = 2;
inline constexpr int64_t kEsrch = 3;
inline constexpr int64_t kEintr = 4;
inline constexpr int64_t kEbadf = 9;
inline constexpr int64_t kEagain = 11;
inline constexpr int64_t kEnomem = 12;
inline constexpr int64_t kEacces = 13;
inline constexpr int64_t kEfault = 14;
inline constexpr int64_t kEbusy = 16;
inline constexpr int64_t kEexist = 17;
inline constexpr int64_t kEnodev = 19;
inline constexpr int64_t kEinval = 22;
inline constexpr int64_t kEnotdir = 20;
inline constexpr int64_t kEisdir = 21;
inline constexpr int64_t kEnotty = 25;
inline constexpr int64_t kEspipe = 29;
inline constexpr int64_t kErofs = 30;
inline constexpr int64_t kEdeadlk = 35;
inline constexpr int64_t kEnametoolong = 36;
inline constexpr int64_t kEnosys = 38;
inline constexpr int64_t kEtimedout = 110;
inline constexpr int64_t kEio = 5;

inline constexpr int64_t kError(int64_t errno_value)
{
	return -errno_value;
}

} // namespace duckemu::runtime::svc
