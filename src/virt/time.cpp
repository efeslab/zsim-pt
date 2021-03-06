/** $glic$
 * Copyright (C) 2017 by Google
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 * Copyright (C) 2011 Google Inc.
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include "log.h"
#include "process_tree.h"
#include "rdtsc.h"
#include "scheduler.h"
#include "virt/common.h"
#include "virt/time_conv.h"
#include "zsim.h"

static bool SkipTimeVirt(PrePatchArgs args) {
    // having both conditions ensures that we don't virtualize in the interim of toggling ff ON
    return args.isNopThread || zinfo->procArray[procIdx]->isInFastForward();
}

// General virtualization functions, used for both syscall and vsyscall/vdso virtualization

void VirtGettimeofday(uint32_t tid, ADDRINT arg0) {
    ZSIM_TRACE(TimeVirt, "[%d] Post-patching gettimeofday", tid);
    if (arg0) {
        struct timeval tv;
        if (!safeCopy((struct timeval*) arg0, &tv)) {
            info("Failed read of gettimeofday() input");
            return;
        }
        ZSIM_TRACE(TimeVirt, "Orig %ld sec, %ld usec", tv.tv_sec, tv.tv_usec);
        uint64_t simNs = cyclesToNs(zinfo->globPhaseCycles);
        uint32_t domain = zinfo->procArray[procIdx]->getClockDomain();
        tv = nsToTimeval(zinfo->clockDomainInfo[domain].realtimeOffsetNs + simNs);

        ZSIM_TRACE(TimeVirt, " Patched %ld sec, %ld usec", tv.tv_sec, tv.tv_usec);
        if (!safeCopy(&tv, (struct timeval*) arg0)) {
            info("Failed write of gettimeofday() output");
        }
    }
}

void VirtTime(uint32_t tid, REG* out, ADDRINT arg0) {
    time_t origRes = (time_t)out;
    if (origRes == ((time_t)-1) || origRes == ((time_t)-EFAULT)) { //glibc will return -1; raw syscall will return -EFAULT
        info("[%d] post-patch time(), returned error or EFAULT (%ld)", tid, origRes);
        return;
    }

    uint64_t simNs = cyclesToNs(zinfo->globPhaseCycles);
    uint32_t domain = zinfo->procArray[procIdx]->getClockDomain();
    time_t tm = (zinfo->clockDomainInfo[domain].realtimeOffsetNs + simNs)/NSPS;

    ZSIM_TRACE(TimeVirt, "[%d] Post-patching time(), orig %ld, new %ld", tid, (time_t)*out, tm);
    *out = (REG)tm;
    if (arg0) {
        if (!safeCopy(&tm, (time_t*) arg0)) {
            info("Failed write of time() output");
        }
    }
}

void VirtClockGettime(uint32_t tid, ADDRINT arg0, ADDRINT arg1) {
    uint32_t domain = zinfo->procArray[procIdx]->getClockDomain();
    ClockDomainInfo& dom =  zinfo->clockDomainInfo[domain];

    //arg0 indicates clock type
    uint64_t offset = 0;
    switch (arg0) {
        case CLOCK_MONOTONIC:
            offset = dom.monotonicOffsetNs;
            break;
        case CLOCK_REALTIME:
            offset = dom.realtimeOffsetNs;
            break;
        case CLOCK_PROCESS_CPUTIME_ID:
            offset = dom.processOffsetNs;
            break;
        case CLOCK_THREAD_CPUTIME_ID:
            offset = dom.processOffsetNs;
            warn("clock_gettime() called with CLOCK_THREAD_CPUTIME_ID, faking with CLOCK_PROCESS_CPUTIME_ID");
            break;
    } //with others, the result does not matter --- actual clock_gettime has returned -1 and EINVAL

    if (arg1) {
        struct timespec ts;
        if (!safeCopy((struct timespec*) arg1, &ts)) {
            info("Failed read of clock_gettime() input");
            return;
        }

        ZSIM_TRACE(TimeVirt, "Patching clock_gettime()");
        ZSIM_TRACE(TimeVirt, "Orig %ld sec, %ld nsec", ts.tv_sec, ts.tv_nsec);

        clock_gettime(CLOCK_MONOTONIC, &ts);
        ZSIM_TRACE(TimeVirt, "MONOTONIC %ld sec, %ld nsec", ts.tv_sec, ts.tv_nsec);
        clock_gettime(CLOCK_REALTIME, &ts);
        ZSIM_TRACE(TimeVirt, "REALTIME %ld sec, %ld nsec", ts.tv_sec, ts.tv_nsec);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
        ZSIM_TRACE(TimeVirt, "PROCESS_CPUTIME_ID %ld sec, %ld nsec", ts.tv_sec, ts.tv_nsec);
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        ZSIM_TRACE(TimeVirt, "THREAD_CPUTIME_ID %ld sec, %ld nsec", ts.tv_sec, ts.tv_nsec);

        uint64_t simNs = cyclesToNs(zinfo->globPhaseCycles);
        ts = nsToTimespec(offset + simNs);
        ZSIM_TRACE(TimeVirt, "Patched %ld sec, %ld nsec", ts.tv_sec, ts.tv_nsec);

        if (!safeCopy(&ts, (struct timespec*) arg1)) {
            info("Failed write of gettimeofday() output");
        }
    }
}

// Syscall patch wrappers

PostPatchFn PatchGettimeofday(PrePatchArgs args) {
    if (SkipTimeVirt(args)) return NullPostPatch;
    return [](PostPatchArgs args) {
        ZSIM_TRACE(TimeVirt, "[%d] Post-patching SYS_gettimeofday", args.tid);
        ADDRINT arg0 = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
        VirtGettimeofday(args.tid, arg0);
        return PPA_NOTHING;
    };
}

PostPatchFn PatchTime(PrePatchArgs args) {
    if (SkipTimeVirt(args)) return NullPostPatch;
    return [](PostPatchArgs args) {
        ZSIM_TRACE(TimeVirt, "[%d] Post-patching SYS_time", args.tid);
        ADDRINT arg0 = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
        REG out = (REG)PIN_GetSyscallNumber(args.ctxt, args.std);
        VirtTime(args.tid, &out, arg0);
        PIN_SetSyscallNumber(args.ctxt, args.std, (ADDRINT) out);  // hack, we have no way of setting the result, this changes rax just as well
        return PPA_NOTHING;
    };
}

PostPatchFn PatchClockGettime(PrePatchArgs args) {
    if (SkipTimeVirt(args)) return NullPostPatch;
    return [](PostPatchArgs args) {
        ZSIM_TRACE(TimeVirt, "[%d] Post-patching SYS_clock_gettime", args.tid);
        ADDRINT arg0 = PIN_GetSyscallArgument(args.ctxt, args.std, 0);
        ADDRINT arg1 = PIN_GetSyscallArgument(args.ctxt, args.std, 1);
        VirtClockGettime(args.tid, arg0, arg1);
        return PPA_NOTHING;
    };
}

// SYS_nanosleep & SYS_clock_nanosleep

PostPatchFn PatchNanosleep(PrePatchArgs args) {
    if (SkipTimeVirt(args)) return NullPostPatch;

    CONTEXT* ctxt = args.ctxt;
    SYSCALL_STANDARD std = args.std;
    uint32_t syscall = PIN_GetSyscallNumber(ctxt, std);
    bool isClock = (syscall == SYS_clock_nanosleep);
    assert(isClock || syscall == SYS_nanosleep);

    struct timespec* ts;
    uint64_t offsetNsec = 0;
    if (isClock) {
        ZSIM_TRACE(TimeVirt, "[%d] Pre-patching SYS_clock_nanosleep", tid);
        int flags = (int) PIN_GetSyscallArgument(ctxt, std, 1);
        ts = (struct timespec*) PIN_GetSyscallArgument(ctxt, std, 2);
        if (flags == TIMER_ABSTIME) {
            ZSIM_TRACE(TimeVirt, "[%d] SYS_clock_nanosleep requests TIMER_ABSTIME, offsetting", tid);
            uint32_t domain = zinfo->procArray[procIdx]->getClockDomain();
            uint64_t simNs = cyclesToNs(zinfo->globPhaseCycles);
            offsetNsec = simNs + zinfo->clockDomainInfo[domain].realtimeOffsetNs;
        }
    } else {
        ZSIM_TRACE(TimeVirt, "[%d] Pre-patching SYS_nanosleep", tid);
        ts = (struct timespec*) PIN_GetSyscallArgument(ctxt, std, 0);
    }

    // Check preconditions
    // FIXME, shouldn't this use safeCopy??
    if (!ts) return NullPostPatch;  // kernel will return EFAULT
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec > 999999999) return NullPostPatch;  // kernel will return EINVAL

    uint64_t waitNsec = timespecToNs(*ts);
    if (waitNsec >= offsetNsec) waitNsec -= offsetNsec;
    else waitNsec = 0;

    uint64_t waitCycles = nsToCycles(waitNsec);
    uint64_t waitPhases = waitCycles/zinfo->phaseLength + 1; //wait at least 1 phase
    uint64_t wakeupPhase = zinfo->numPhases + waitPhases;

    volatile uint32_t* futexWord = zinfo->sched->markForSleep(procIdx, args.tid, wakeupPhase);

    // Save args
    ADDRINT arg0 = PIN_GetSyscallArgument(ctxt, std, 0);
    ADDRINT arg1 = PIN_GetSyscallArgument(ctxt, std, 1);
    ADDRINT arg2 = PIN_GetSyscallArgument(ctxt, std, 2);
    ADDRINT arg3 = PIN_GetSyscallArgument(ctxt, std, 3);
    struct timespec* rem = (struct timespec*) PIN_GetSyscallArgument(ctxt, std, isClock? 3 : 1);

    // Turn this into a non-timed FUTEX_WAIT syscall
    PIN_SetSyscallNumber(ctxt, std, SYS_futex);
    PIN_SetSyscallArgument(ctxt, std, 0, (ADDRINT)futexWord);
    PIN_SetSyscallArgument(ctxt, std, 1, (ADDRINT)FUTEX_WAIT);
    PIN_SetSyscallArgument(ctxt, std, 2, (ADDRINT)1 /*by convention, see sched code*/);
    PIN_SetSyscallArgument(ctxt, std, 3, (ADDRINT)nullptr);

    return [isClock, wakeupPhase, arg0, arg1, arg2, arg3, rem](PostPatchArgs args) {
        CONTEXT* ctxt = args.ctxt;
        SYSCALL_STANDARD std = args.std;

        if (isClock) {
            ZSIM_TRACE(TimeVirt, "[%d] Post-patching SYS_clock_nanosleep", tid);
        } else {
            ZSIM_TRACE(TimeVirt, "[%d] Post-patching SYS_nanosleep", tid);
        }

        int res = (int)(-PIN_GetSyscallNumber(ctxt, std));
        if (res == EWOULDBLOCK) {
            ZSIM_TRACE(TimeVirt, "Fixing EWOULDBLOCK --> 0");
            PIN_SetSyscallNumber(ctxt, std, 0);  // this is fine, you just called a very very short sleep
        } else if (res == EINTR) {
            PIN_SetSyscallNumber(ctxt, std, -EINTR);  // we got an interrupt
        } else {
            ZSIM_TRACE(TimeVirt, "%d", res);
            assert(res == 0);
        }

        // Restore pre-call args
        PIN_SetSyscallArgument(ctxt, std, 0, arg0);
        PIN_SetSyscallArgument(ctxt, std, 1, arg1);
        PIN_SetSyscallArgument(ctxt, std, 2, arg2);
        PIN_SetSyscallArgument(ctxt, std, 3, arg3);

        // Handle remaining time stuff
        if (rem) {
            if (res == EINTR) {
                assert(wakeupPhase >= zinfo->numPhases);  // o/w why is this EINTR...
                uint64_t remainingCycles = wakeupPhase - zinfo->numPhases;
                uint64_t remainingNsecs = remainingCycles*1000/zinfo->freqMHz;
                rem->tv_sec = remainingNsecs/1000000000;
                rem->tv_nsec = remainingNsecs % 1000000000;
            } else {
                rem->tv_sec = 0;
                rem->tv_nsec = 0;
            }
        }

        return PPA_NOTHING;
    };
}

// Clock domain query functions

void VirtCaptureClocks(bool isDeffwd) {
    uint32_t domain = zinfo->procArray[procIdx]->getClockDomain();
    ClockDomainInfo& dom = zinfo->clockDomainInfo[domain];
    futex_lock(&dom.lock);
    if (isDeffwd || dom.realtimeOffsetNs == 0) {
        info("[%d] Adjusting clocks, domain %d, de-ffwd %d", procIdx, domain, isDeffwd);

        struct timespec realtime;
        struct timespec monotonic;
        struct timespec process;
        clock_gettime(CLOCK_REALTIME, &realtime);
        clock_gettime(CLOCK_MONOTONIC, &monotonic);
        clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &process);
        uint64_t realRdtsc = rdtsc();

        uint64_t curCycles = zinfo->globPhaseCycles;
        uint64_t curNs = cyclesToNs(curCycles);

        uint64_t realtimeNs = timespecToNs(realtime);
        uint64_t monotonicNs = timespecToNs(monotonic);
        uint64_t processNs = timespecToNs(process);

        dom.realtimeOffsetNs = realtimeNs - curNs;
        dom.monotonicOffsetNs = monotonicNs - curNs;
        dom.processOffsetNs = processNs - curNs;
        dom.rdtscOffset = realRdtsc - curCycles;

        //info("Offsets: %ld %ld %ld %ld", dom.realtimeOffsetNs, dom.monotonicOffsetNs, dom.processOffsetNs, dom.rdtscOffset)
    }
    futex_unlock(&dom.lock);
}

uint64_t VirtGetPhaseRDTSC() {
    uint32_t domain = zinfo->procArray[procIdx]->getClockDomain();
    return zinfo->clockDomainInfo[domain].rdtscOffset + zinfo->globPhaseCycles;
}

// SYS_alarm

PostPatchFn PatchAlarmSyscall(PrePatchArgs args) {
    if (SkipTimeVirt(args)) return NullPostPatch;

    CONTEXT* ctxt = args.ctxt;
    SYSCALL_STANDARD std = args.std;
    uint32_t syscall = PIN_GetSyscallNumber(ctxt, std);
    assert(syscall == SYS_alarm);
    ZSIM_TRACE(TimeVirt, "Patching SYS_alarm");
    unsigned int secs = (unsigned int) PIN_GetSyscallArgument(ctxt, std, 0);
    unsigned int secsRemain = zinfo->sched->intervalTimer.setAlarm(getpid(), secs);

    //Turn this into a NOP by setting the argument to 0 (clears old timers)
    PIN_SetSyscallArgument(ctxt, std, 0, 0);

    //Postpatch to restore the arg and set the return value
    return [secs, secsRemain](PostPatchArgs args) {
        CONTEXT* ctxt = args.ctxt;
        SYSCALL_STANDARD std = args.std;

        //Restore pre-call argument
        PIN_SetSyscallArgument(ctxt, std, 0, secs);

        //Set the return value in rax
        PIN_REGISTER reg;
        reg.dword[0] = secsRemain;
        reg.dword[1] = 0;
        PIN_SetContextRegval(ctxt, LEVEL_BASE::REG_EAX, (UINT8*)&reg);

        return PPA_NOTHING;
    };
}

// SYS_getitimer

PostPatchFn PatchGetitimerSyscall(PrePatchArgs args) {
    return NullPostPatch;
}

// SYS_setitimer

PostPatchFn PatchSetitimerSyscall(PrePatchArgs args) {
    if (SkipTimeVirt(args)) return NullPostPatch;

    CONTEXT* ctxt = args.ctxt;
    SYSCALL_STANDARD std = args.std;
    uint32_t syscall = PIN_GetSyscallNumber(ctxt, std);
    assert(syscall == SYS_setitimer);
    ZSIM_TRACE(TimeVirt, "Patching SYS_setitimer");

    //Grab new and old itimerval args
    ADDRINT arg0 = PIN_GetSyscallArgument(ctxt, std, 0);
    ADDRINT arg1 = PIN_GetSyscallArgument(ctxt, std, 1);
    struct itimerval* newVal = new struct itimerval();
    PIN_SafeCopy(newVal, (void *)arg1, sizeof(struct itimerval));
    struct itimerval* oldVal = new struct itimerval();
    int res = zinfo->sched->intervalTimer.setIntervalTimer(getpid(), (int)arg0, newVal, oldVal);

    //Turn this into a NOP by disabling the timer
    memset(newVal, 0, sizeof(struct itimerval));
    PIN_SetSyscallArgument(ctxt, std, 1, (ADDRINT)newVal);

    //Postpatch to set newVal, oldVal and the return value
    return [arg1, newVal, oldVal, res](PostPatchArgs args) {
        CONTEXT* ctxt = args.ctxt;
        SYSCALL_STANDARD std = args.std;

        //Reset newVal
        PIN_SetSyscallArgument(ctxt, std, 1, arg1);

        //Set oldVal and free memory
        ADDRINT arg2 = PIN_GetSyscallArgument(ctxt, std, 2);
        PIN_SafeCopy((void *)arg2, oldVal, sizeof(struct itimerval));
        delete newVal;
        delete oldVal;

        //Set return value in rax
        PIN_REGISTER reg;
        reg.dword[0] = res;
        reg.dword[1] = 0;
        PIN_SetContextRegval(ctxt, LEVEL_BASE::REG_EAX, (UINT8*)&reg);

        return PPA_NOTHING;
    };
}
