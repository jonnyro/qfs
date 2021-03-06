//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2010/11/02
// Author: Mike Ovsiannikov
//
// Copyright 2010 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// Compile and link against this to replace global operator new and delete,
// and trace / debug memory allocation.
//
//----------------------------------------------------------------------------

#include <new>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <fcntl.h>

#ifdef KFS_OS_NAME_LINUX
#include <malloc.h>
#include <execinfo.h>
#else
struct mallinfo {};
#endif

struct KfsTraceNew
{
    KfsTraceNew()
        : mParamsSetFlag(false),
          mFailedSize(0),
          mMapsSize(0),
          mTraceFd(-1),
          mMaxNewSize(0),
          mContinueOnMallocFailureFlag(false),
          mParametersPtr(getenv("KFS_TRACE_NEW_PARAMS")),
          mStackTraceDepth(0),
          mRlimAs(),
          mRlimData(),
          mRlimCore(),
          mMallinfo()
    {
        // Format: fd,maxsize,abort
        // Example: KFS_TRACE_NEW_PARAMS=2,1e3,1

        mMapsBuf[0] = 0;
        if (! mParametersPtr || ! *mParametersPtr) {
            return;
        }
        char* theEndPtr = 0;
        mTraceFd = (int)strtol(mParametersPtr, &theEndPtr, 0);
        if ((*theEndPtr & 0xFF) == ',') {
            const char* thePtr = theEndPtr + 1;
            theEndPtr = 0;
            mMaxNewSize = (size_t)strtod(thePtr, &theEndPtr);
        }
        if ((*theEndPtr & 0xFF) == ',') {
            const char* thePtr = theEndPtr + 1;
            theEndPtr = 0;
            mContinueOnMallocFailureFlag = strtol(thePtr, &theEndPtr, 0) == 0;
        }
        mParamsSetFlag = true;
    }
    void Trace(
        const char* inMsgPtr,
        size_t      inSize,
        const void* inPtr)
    {
        if (mTraceFd < 0 || ! mParamsSetFlag) {
            return;
        }
        char theBuf[64];
        const int theLen = snprintf(theBuf, sizeof(theBuf),
            "%s %lu %p\n", inMsgPtr, (unsigned long)inSize, inPtr);
        if (theLen <= 0) {
            return;
        }
        write(mTraceFd, theBuf, theLen);
    }
    void MallocFailed(
        size_t inSize);
    void* Allocate(
        size_t inSize,
        bool   inRaiseExceptionOnFailureFlag)
    {
        void* const thePtr = (mMaxNewSize <= 0 || inSize <= mMaxNewSize) ?
            malloc(inSize) : 0;
        Trace(inRaiseExceptionOnFailureFlag ? "new" : "new_nt", inSize, thePtr);
        if (! thePtr) {
            MallocFailed(inSize);
            if (inRaiseExceptionOnFailureFlag) {
                throw std::bad_alloc();
            }
        }
        return thePtr;
    }
    void Free(
        const char* inMsgPtr,
        void*       inPtr)
    {
        Trace(inMsgPtr, -1, inPtr);
        free(inPtr);
    }

    enum { kMaxStackTraceDepth = 64 };

    bool              mParamsSetFlag;
    size_t            mFailedSize;
    int               mMapsSize;
    int               mTraceFd;
    size_t            mMaxNewSize;
    bool              mContinueOnMallocFailureFlag;
    const char* const mParametersPtr;
    int               mStackTraceDepth;
    struct rlimit     mRlimAs;
    struct rlimit     mRlimData;
    struct rlimit     mRlimCore;
    struct mallinfo   mMallinfo;
    char              mMapsBuf[16 << 10];
    void*             mStackTrace[kMaxStackTraceDepth];
} gKfsTraceNew;

void
KfsTraceNew::MallocFailed(
    size_t inSize)
{
    getrlimit(RLIMIT_AS,   &mRlimAs);
    getrlimit(RLIMIT_DATA, &mRlimData);
    getrlimit(RLIMIT_CORE, &mRlimCore);
#ifdef KFS_OS_NAME_LINUX
    struct mallinfo info = mallinfo();
    mFailedSize = inSize;
    mMallinfo   = info;
    const int theFd = open("/proc/self/maps", O_RDONLY);
    int theMapsSize = 0;
    if (theFd >= 0) {
        theMapsSize = (int)read(theFd, mMapsBuf, sizeof(mMapsBuf));
        mMapsSize = theMapsSize;
        close(theFd);
    }
    void* theStackTrace[kMaxStackTraceDepth];
    const int theDepth = backtrace(theStackTrace, kMaxStackTraceDepth);
    if (theDepth > 0) {
        char theBuf[64];
        const int theLen = snprintf(theBuf, sizeof(theBuf),
            "malloc(%lu) failure:\n", (unsigned long)inSize);
        if (theLen > 0) {
            write(2, theBuf, theLen);
        }
        backtrace_symbols_fd(theStackTrace, theDepth, 2);
        memcpy(mStackTrace, theStackTrace, theDepth * sizeof(mStackTrace[0]));
        if (theMapsSize > 0) {
            write(2, mMapsBuf, theMapsSize);
        }
        mStackTraceDepth = theDepth;
    }
#endif
    if (mContinueOnMallocFailureFlag) {
        return;
    }
    abort();
}

void*
operator new(std::size_t inSize) throw (std::bad_alloc)
{
    return gKfsTraceNew.Allocate(inSize, true);
}

void
operator delete(void* inPtr) throw()
{
    gKfsTraceNew.Free("delete", inPtr);
}

void*
operator new(std::size_t inSize, const std::nothrow_t&) throw()
{
    return gKfsTraceNew.Allocate(inSize, false);
}

void
operator delete(void* inPtr, const std::nothrow_t&) throw()
{
    gKfsTraceNew.Free("delete_nt", inPtr);
}
