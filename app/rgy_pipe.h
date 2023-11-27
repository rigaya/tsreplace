// -----------------------------------------------------------------------------------------
// QSVEnc/NVEnc by rigaya
// -----------------------------------------------------------------------------------------
// The MIT License
//
// Copyright (c) 2011-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// --------------------------------------------------------------------------------------------

#pragma once
#ifndef __RGY_PIPE_H__
#define __RGY_PIPE_H__

#include <cstdint>
#include <cstdio>
#include <vector>
#include "rgy_osdep.h"
#include "rgy_util.h"
#include "rgy_tchar.h"

enum PipeMode {
    PIPE_MODE_DISABLE = 0,
    PIPE_MODE_ENABLE,
    PIPE_MODE_MUXED, //Stderrのモードに使用し、StderrをStdOutに混合する
};

static const int QSV_PIPE_READ_BUF = 2048;

#if defined(_WIN32) || defined(_WIN64)
typedef HANDLE PIPE_HANDLE;
typedef HANDLE PROCESS_HANDLE;
#else
typedef int PIPE_HANDLE;
typedef pid_t PROCESS_HANDLE;
#endif

typedef struct {
    PIPE_HANDLE h_read;
    PIPE_HANDLE h_write;
    PipeMode mode;
    uint32_t bufferSize;
} PipeSet;

typedef struct {
    PipeSet stdIn;
    PipeSet stdOut;
    PipeSet stdErr;
    FILE *f_stdin;
} ProcessPipe;

class RGYPipeProcess {
public:
    RGYPipeProcess() : m_phandle(0), m_pipe(){ };
    virtual ~RGYPipeProcess() { };

    void init(PipeMode stdin_, PipeMode stdout_, PipeMode stderr_) {
        m_pipe.stdIn.mode = stdin_;
        m_pipe.stdOut.mode = stdout_;
        m_pipe.stdErr.mode = stderr_;
    };
    virtual int run(const std::vector<const TCHAR *>& args, const TCHAR *exedir, uint32_t priority, bool hidden, bool minimized) = 0;
    virtual void close() = 0;
    virtual bool processAlive() = 0;
    virtual std::string getOutput() = 0;
    virtual int stdOutRead(std::vector<uint8_t>& buffer) = 0;
    virtual int stdErrRead(std::vector<uint8_t>& buffer) = 0;
    virtual size_t stdInWrite(const void *data, const size_t dataSize) = 0;
    virtual int stdInFlush() = 0;
    virtual int stdInClose() = 0;
protected:
    virtual int startPipes() = 0;
    PROCESS_HANDLE m_phandle;
    ProcessPipe m_pipe;
};

#if defined(_WIN32) || defined(_WIN64)
class RGYPipeProcessWin : public RGYPipeProcess {
public:
    RGYPipeProcessWin();
    virtual ~RGYPipeProcessWin();

    virtual int run(const std::vector<const TCHAR *>& args, const TCHAR *exedir, uint32_t priority, bool hidden, bool minimized) override;
    virtual void close() override;
    virtual bool processAlive() override;
    virtual std::string getOutput() override;
    virtual int stdOutRead(std::vector<uint8_t>& buffer) override;
    virtual int stdErrRead(std::vector<uint8_t>& buffer) override;
    virtual size_t stdInWrite(const void *data, const size_t dataSize) override;
    virtual int stdInFlush() override;
    virtual int stdInClose() override;
    const PROCESS_INFORMATION& getProcessInfo();
protected:
    virtual int startPipes() override;
    PROCESS_INFORMATION m_pi;
};
#else
class RGYPipeProcessLinux : public RGYPipeProcess {
public:
    RGYPipeProcessLinux();
    virtual ~RGYPipeProcessLinux();

    virtual int run(const std::vector<const TCHAR *>& args, const TCHAR *exedir, uint32_t priority, bool hidden, bool minimized) override;
    virtual void close() override;
    virtual bool processAlive() override;
    virtual std::string getOutput() override;
    virtual int stdOutRead(std::vector<uint8_t>& buffer) override;
    virtual int stdErrRead(std::vector<uint8_t>& buffer) override;
    virtual size_t stdInWrite(const void *data, const size_t dataSize) override;
    virtual int stdInFlush() override;
    virtual int stdInClose() override;
protected:
    virtual int startPipes() override;
};
#endif //#if defined(_WIN32) || defined(_WIN64)

std::unique_ptr<RGYPipeProcess> createRGYPipeProcess();

#endif //__RGY_PIPE_H__
