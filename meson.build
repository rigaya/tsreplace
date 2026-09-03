project('tsreplace', ['c', 'cpp'],
  version: run_command('bash', 'scripts/get-version.sh', check: true).stdout().strip(),
  license: 'MIT',
  meson_version: '>=0.61.0',
  default_options: [
    'cpp_std=c++17',
    'buildtype=release',
    'warning_level=1',
  ]
)

cpp = meson.get_compiler('cpp')

if host_machine.system() != 'linux'
  error('This meson.build is for Linux only')
endif

libav_static = get_option('libav_static')
static_runtime = get_option('static_runtime')
static_runtime_link_args = []
if static_runtime
  static_runtime_link_args = cpp.get_supported_link_arguments([
    '-static-libstdc++',
    '-static-libgcc',
  ])
endif

threads_dep = dependency('threads')
dl_dep = cpp.find_library('dl', required: true)
m_dep = cpp.find_library('m', required: true)
stdcxxfs_dep = cpp.find_library('stdc++fs', required: false)

libavutil_dep = dependency('libavutil', static: libav_static, required: true)
libavcodec_dep = dependency('libavcodec', static: libav_static, required: true)
libavformat_dep = dependency('libavformat', static: libav_static, required: true)
libavfilter_dep = dependency('libavfilter', static: libav_static, required: true)
libswresample_dep = dependency('libswresample', static: libav_static, required: true)

ffmpeg_deps = [
  libavutil_dep,
  libavcodec_dep,
  libavformat_dep,
  libavfilter_dep,
  libswresample_dep,
]

conf_data = configuration_data()

av_channel_layout_code = '''
#include <libavutil/channel_layout.h>
int main(void) { AVChannelLayout ch_layout; return 0; }
'''
av_channel_layout_struct_avail = cpp.compiles(av_channel_layout_code, dependencies: libavutil_dep)

av_frame_duration_code = '''
#include <libavutil/frame.h>
int main(void) { AVFrame *frame; frame->duration = 0; return 0; }
'''
av_frame_duration_avail = cpp.compiles(av_frame_duration_code, dependencies: libavutil_dep)

avcodec_par_side_data_code = '''
#include <libavcodec/codec_par.h>
int main(void) { AVCodecParameters *codecpar; codecpar->coded_side_data = 0; return 0; }
'''
avcodec_par_coded_side_data_avail = cpp.compiles(avcodec_par_side_data_code, dependencies: libavcodec_dep)

conf_data.set10('ENABLE_RAW_READER', false)
conf_data.set10('ENABLE_AVI_READER', false)
conf_data.set10('ENABLE_AVISYNTH_READER', false)
conf_data.set10('ENABLE_VAPOURSYNTH_READER', false)
conf_data.set10('ENABLE_AVSW_READER', true)
conf_data.set10('ENABLE_SM_READER', false)
conf_data.set10('ENABLE_CUSTOM_VPP', true)
conf_data.set10('ENABLE_LIBASS_SUBBURN', false)
conf_data.set10('ENABLE_ADVANCED_DEINTERLACE', false)
conf_data.set10('ENABLE_MVC_ENCODING', false)
conf_data.set10('ENABLE_FPS_CONVERSION', false)
conf_data.set10('ENABLE_OPENCL', false)
conf_data.set10('ENABLE_SESSION_THREAD_CONFIG', false)
conf_data.set10('ENABLE_AVCODEC_OUT_THREAD', true)
conf_data.set10('ENABLE_AVCODEC_AUDPROCESS_THREAD', true)
conf_data.set10('ENABLE_LIBAVDEVICE', false)
conf_data.set10('ENABLE_LIBDOVI', false)
conf_data.set10('ENABLE_LIBHDR10PLUS', false)
conf_data.set10('ENABLE_VULKAN', false)
conf_data.set10('ENABLE_LIBPLACEBO', false)
conf_data.set10('AV_CHANNEL_LAYOUT_STRUCT_AVAIL', av_channel_layout_struct_avail)
conf_data.set10('AV_FRAME_DURATION_AVAIL', av_frame_duration_avail)
conf_data.set10('AVCODEC_PAR_CODED_SIDE_DATA_AVAIL', avcodec_par_coded_side_data_avail)
conf_data.set10('ENABLE_CPP_REGEX', true)
conf_data.set10('ENABLE_DTL', false)

configure_file(
  output: 'rgy_config.h',
  configuration: conf_data,
)

rev_count = run_command('sh', '-c', 'cd "' + meson.project_source_root() + '" && git rev-list HEAD | wc -l', check: false).stdout().strip()
if rev_count == ''
  rev_count = '0'
endif

rev_data = configuration_data()
rev_data.set_quoted('ENCODER_REV', rev_count)
rev_data.set('ENABLE_METRIC_FRAMEWORK', 0)
configure_file(
  output: 'rgy_rev.h',
  configuration: rev_data,
)

common_cpp_args = [
  '-DLINUX',
  '-DUNIX',
  '-D_FILE_OFFSET_BITS=64',
  '-D__USE_LARGEFILE64',
  '-D__STDC_CONSTANT_MACROS',
  '-D__STDC_FORMAT_MACROS',
  '-Wno-unknown-pragmas',
  '-Wno-unused',
  '-Wno-missing-braces',
] + get_option('cpp_args')

common_c_args = [
  '-DLINUX',
  '-DUNIX',
  '-D_FILE_OFFSET_BITS=64',
  '-D__USE_LARGEFILE64',
  '-D__STDC_CONSTANT_MACROS',
  '-D__STDC_FORMAT_MACROS',
  '-Wno-unknown-pragmas',
  '-Wno-unused',
  '-Wno-missing-braces',
]

if host_machine.cpu_family() in ['x86_64', 'aarch64']
  common_cpp_args += ['-DLINUX64']
  common_c_args += ['-DLINUX64']
else
  common_cpp_args += ['-DLINUX32']
  common_c_args += ['-DLINUX32']
endif

no_rdtscp_intrin = false
if host_machine.cpu_family() == 'x86_64'
  rdtscp_code = '''
  #include <x86intrin.h>
  int main(void) {
    unsigned int aux = 0;
    auto i = __rdtscp(&aux);
    return (int)i;
  }
  '''
  no_rdtscp_intrin = not cpp.compiles(rdtscp_code)
endif
if no_rdtscp_intrin
  common_cpp_args += ['-DNO_RDTSCP_INTRIN=1']
endif

common_include_dirs = include_directories('.', 'app')

tsreplace_sources = files(
  'app/cpu_info.cpp',
  'app/gpu_info.cpp',
  'app/rgy_avlog.cpp',
  'app/rgy_avutil.cpp',
  'app/rgy_bitstream.cpp',
  'app/rgy_bitstream_aac.cpp',
  'app/rgy_codepage.cpp',
  'app/rgy_def.cpp',
  'app/rgy_env.cpp',
  'app/rgy_err.cpp',
  'app/rgy_event.cpp',
  'app/rgy_filesystem.cpp',
  'app/rgy_log.cpp',
  'app/rgy_memmem.cpp',
  'app/rgy_pipe.cpp',
  'app/rgy_pipe_linux.cpp',
  'app/rgy_simd.cpp',
  'app/rgy_tsdemux.cpp',
  'app/rgy_tscut.cpp',
  'app/rgy_tsstruct.cpp',
  'app/rgy_tsutil.cpp',
  'app/rgy_util.cpp',
  'app/rgy_version.cpp',
  'app/tsreplace.cpp',
)

simd_libs = []
if host_machine.cpu_family() == 'x86_64'
  simd_avx2_lib = static_library('tsreplace_simd_avx2',
    [
      'app/rgy_bitstream_avx2.cpp',
      'app/rgy_memmem_avx2.cpp',
    ],
    cpp_args: common_cpp_args + ['-mavx2', '-mpopcnt', '-mbmi', '-mbmi2'],
    include_directories: common_include_dirs,
  )

  simd_avx512bw_lib = static_library('tsreplace_simd_avx512bw',
    [
      'app/rgy_bitstream_avx512bw.cpp',
      'app/rgy_memmem_avx512bw.cpp',
    ],
    cpp_args: common_cpp_args + ['-mavx512f', '-mavx512bw', '-mpopcnt', '-mbmi', '-mbmi2'],
    include_directories: common_include_dirs,
  )

  simd_libs += [simd_avx2_lib, simd_avx512bw_lib]
endif

all_deps = [
  threads_dep,
  dl_dep,
  m_dep,
] + ffmpeg_deps

if stdcxxfs_dep.found()
  all_deps += stdcxxfs_dep
endif

executable('tsreplace',
  tsreplace_sources,
  c_args: common_c_args,
  cpp_args: common_cpp_args,
  include_directories: common_include_dirs,
  dependencies: all_deps,
  link_args: static_runtime_link_args,
  link_with: simd_libs,
  install: true,
)

tscut_test_deps = []
if stdcxxfs_dep.found()
  tscut_test_deps += stdcxxfs_dep
endif

tscut_test = executable('rgy_tscut_test',
  files(
    'app/rgy_bitstream_aac.cpp',
    'app/rgy_tscut.cpp',
    'app/rgy_tscut_test.cpp',
  ),
  cpp_args: common_cpp_args,
  include_directories: common_include_dirs,
  dependencies: tscut_test_deps,
  link_args: static_runtime_link_args,
)

test('rgy_tscut', tscut_test)

summary({
  'FFmpeg static deps': libav_static,
  'Static runtime': static_runtime,
  'AVChannelLayout': av_channel_layout_struct_avail,
  'AVFrame.duration': av_frame_duration_avail,
  'AVCodecParameters.coded_side_data': avcodec_par_coded_side_data_avail,
  'OpenCL helpers': false,
  'x86_64 SIMD': host_machine.cpu_family() == 'x86_64',
  'NO_RDTSCP_INTRIN': no_rdtscp_intrin,
}, section: 'tsreplace Features')
