// -----------------------------------------------------------------------------------------
// QSVEnc by rigaya
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

#include "rgy_tchar.h"
#include <string>
#include <vector>
#include <random>
#include <future>
#include <algorithm>
#include "rgy_osdep.h"
#include "rgy_util.h"
#include "gpu_info.h"

#pragma warning (push)
#pragma warning (disable: 4100)
int getGPUInfo(const char *VendorName, TCHAR *buffer, const unsigned int buffer_size, const int adapterID, RGYOpenCLPlatform *clplatform) {
#if LIBVA_SUPPORT
    _stprintf_s(buffer, buffer_size, _T("Intel Graphics / Driver : %s"), getGPUInfoVA().c_str());
    return 0;
#elif ENABLE_OPENCL

#if !FOR_AUO
    IntelDeviceInfo info = { 0 };
    const auto intelInfoRet = getIntelGPUInfo(&info, adapterID);
    IntelDeviceInfo* intelinfoptr = (intelInfoRet == 0) ? &info : nullptr;
#else
    IntelDeviceInfo* intelinfoptr = nullptr;
#endif


    RGYOpenCLDeviceInfo clinfo;
    if (clplatform) {
        clinfo = clplatform->dev(0).info();
    } else {
#if !FOR_AUO
        auto clinfoqsv = getDeviceCLInfoQSV((QSVDeviceNum)adapterID);
        if (clinfoqsv.has_value()) {
            clinfo = clinfoqsv.value();
        }
#else
        RGYOpenCL cl;
        auto platforms = cl.getPlatforms(VendorName);
        for (auto& p : platforms) {
            if (p->createDeviceList(CL_DEVICE_TYPE_GPU) == RGY_ERR_NONE && p->devs().size() > 0) {
                clinfo = p->dev(0).info();
                break;
            }
        }
#endif
    }
    cl_create_info_string((clinfo.name.length() > 0) ? &clinfo : nullptr, intelinfoptr, buffer, buffer_size);
    return 0;
#else
    buffer[0] = _T('\0');
    return 1;
#endif // !ENABLE_OPENCL
}
#pragma warning (pop)
