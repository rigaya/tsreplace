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

#if !(defined(_WIN32) || defined(_WIN64))
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <chrono>
#include "rgy_pipe.h"
#include "rgy_tchar.h"

RGYPipeProcessLinux::RGYPipeProcessLinux() {
}

RGYPipeProcessLinux::~RGYPipeProcessLinux() {

}

int RGYPipeProcessLinux::startPipes() {
    if (m_pipe.stdOut.mode) {
        if (-1 == (pipe((int *)&m_pipe.stdOut.h_read)))
            return 1;
        //m_pipe.f_stdout = fdopen(m_pipe.stdOut.h_read, "r");
    }
    if (m_pipe.stdErr.mode) {
        if (-1 == (pipe((int *)&m_pipe.stdErr.h_read)))
            return 1;
        //m_pipe.f_stderr = fdopen(m_pipe.stdErr.h_read, "r");
    }
    if (m_pipe.stdIn.mode) {
        if (-1 == (pipe((int *)&m_pipe.stdIn.h_read)))
            return 1;
        m_pipe.f_stdin = fdopen(m_pipe.stdIn.h_write, "w");
    }
    return 0;
}

int RGYPipeProcessLinux::run(const std::vector<const TCHAR *>& args, const TCHAR *exedir, uint32_t priority, bool hidden, bool minimized) {
    startPipes();

    m_phandle = fork();
    if (m_phandle < 0) {
        return 1;
    }

    if (m_phandle == 0) {
        //子プロセス
        if (m_pipe.stdIn.mode) {
            ::close(m_pipe.stdIn.h_write);
            dup2(m_pipe.stdIn.h_read, STDIN_FILENO);
        }
        if (m_pipe.stdOut.mode) {
            ::close(m_pipe.stdOut.h_read);
            dup2(m_pipe.stdOut.h_write, STDOUT_FILENO);
        }
        if (m_pipe.stdErr.mode) {
            ::close(m_pipe.stdErr.h_read);
            dup2(m_pipe.stdErr.h_write, STDERR_FILENO);
        }
        int ret = execvp(args[0], (char *const *)args.data());
        exit(-1);
    }
    //親プロセス
    if (m_pipe.stdIn.mode) {
        ::close(m_pipe.stdIn.h_read);
        m_pipe.stdIn.h_read = 0;
    }
    if (m_pipe.stdOut.mode) {
        ::close(m_pipe.stdOut.h_write);
        m_pipe.stdOut.h_write = 0;
    }
    if (m_pipe.stdErr.mode) {
        ::close(m_pipe.stdErr.h_write);
        m_pipe.stdErr.h_write = 0;
    }
    return 0;
}

void RGYPipeProcessLinux::close() {
    if (m_pipe.f_stdin) {
        fclose(m_pipe.f_stdin);
        m_pipe.f_stdin = nullptr;
        m_pipe.stdIn.h_write = 0;
    }
    if (m_pipe.stdIn.h_read) {
        ::close(m_pipe.stdIn.h_read);
        m_pipe.stdIn.h_read = 0;
    }
    if (m_pipe.stdIn.h_write) {
        ::close(m_pipe.stdIn.h_write);
        m_pipe.stdIn.h_write = 0;
    }
    if (m_pipe.stdOut.h_read) {
        ::close(m_pipe.stdOut.h_read);
        m_pipe.stdOut.h_read = 0;
    }
    if (m_pipe.stdOut.h_write) {
        ::close(m_pipe.stdOut.h_write);
        m_pipe.stdOut.h_write = 0;
    }
    if (m_pipe.stdErr.h_read) {
        ::close(m_pipe.stdErr.h_read);
        m_pipe.stdErr.h_read = 0;
    }
    if (m_pipe.stdErr.h_write) {
        ::close(m_pipe.stdErr.h_write);
        m_pipe.stdErr.h_write = 0;
    }
}

size_t RGYPipeProcessLinux::stdInWrite(const void *data, const size_t dataSize) {
    return fwrite(data, 1, dataSize, m_pipe.f_stdin);
}

int RGYPipeProcessLinux::stdInFlush() {
    return fflush(m_pipe.f_stdin);
}

int RGYPipeProcessLinux::stdInClose() {
    return fclose(m_pipe.f_stdin);
}

int RGYPipeProcessLinux::stdOutRead(std::vector<uint8_t>& buffer) {
    auto read_from_pipe = [&]() {
        char read_buf[512 * 1024];
        int pipe_read = (int)read(m_pipe.stdOut.h_read, read_buf, _countof(read_buf));
        if (pipe_read == -1) return -1;
        buffer.insert(buffer.end(), read_buf, read_buf + pipe_read);
        return (int)pipe_read;
    };

    int ret = 0;
    for (;;) {
        ret = read_from_pipe();
        if (ret != 0) {
            break;
        }
    }
    return ret < 0 ? -1 : (int)buffer.size();
}
int RGYPipeProcessLinux::stdErrRead(std::vector<uint8_t>& buffer) {
    auto read_from_pipe = [&]() {
        char read_buf[4096];
        int pipe_read = (int)read(m_pipe.stdErr.h_read, read_buf, _countof(read_buf));
        if (pipe_read == -1) return -1;
        buffer.insert(buffer.end(), read_buf, read_buf + pipe_read);
        return (int)pipe_read;
    };

    int ret = 0;
    for (;;) {
        ret = read_from_pipe();
        if (ret != 0) {
            break;
        }
    }
    return ret < 0 ? -1 : (int)buffer.size();
}

tstring RGYPipeProcessLinux::getOutput() {
    std::string outstr;
    auto read_from_pipe = [&]() {
        char read_buf[4096];
        int ret = (int)read(m_pipe.stdOut.h_read, read_buf, _countof(read_buf));
        if (ret == -1) return -1;
        outstr += std::string(read_buf, read_buf+ret);
        return (int)ret;
    };
    for (;;) {
        if (read_from_pipe() < 0) {
            break;
        }
    }
    return outstr;
}

bool RGYPipeProcessLinux::processAlive() {
    int status = 0;
    return 0 == waitpid(m_phandle, &status, WNOHANG);
}
#endif //#if !(defined(_WIN32) || defined(_WIN64))
