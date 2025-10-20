project('jimage', 'c', default_options: ['c_std=c99', 'warning_level=3'], version: run_command('tools/version.py', check: true).stdout().strip(), meson_version: '>=1.1.0')


subdir('jimage/config')


deps = []
deps += [dependency('', fallback: ['ctoolbox', 'ctoolbox_dep'])]
deps += [dependency('', fallback: ['jdeflate', 'jdeflate_dep'])]


cc = meson.get_compiler('c')
if cc.get_id() == 'msvc'
  add_project_arguments('-D__MSVC__', language: 'c')
else
  if cc.get_id() == 'gcc' or cc.get_id() == 'clang'
    compilerflags = [
      '-Wconversion',
      '-Wcast-qual',
      '-Wunused-parameter',
      '-Wshadow',
      '-Wswitch',
      '-Wcast-align',
      '-Wdouble-promotion'
    ]
    add_project_arguments(compilerflags, language: 'c')
  endif
endif


projectsources = []


isunix    = false
iswindows = false

# System platform
if target_machine.system() in ['windows', 'cygwin']
  iswindows = true
else
  if target_machine.system() in ['android', 'darwin', 'dragonfly', 'freebsd', 'gnu', 'linux', 'netbsd', 'openbsd', 'sunos']
    isunix = true
  endif
endif


target = target_machine.cpu_family()
if target == 'x86_64'
  supported = false

  path = 'src/asm/'
  if isunix
    supported = true
  endif
  if iswindows
    supported = true
  endif

  if supported
    if add_languages('nasm', required: false, native: false)
      if isunix
        if host_machine.system() in ['darwin', 'ios', 'tvos']
          nasmformat = 'macho64'
        else
          nasmformat = 'elf64'
        endif
      endif
      if iswindows
        nasmformat = 'win64'
      endif

      add_project_arguments('-f', nasmformat, language: 'nasm')
      projectsources += [path / 'x64-jpgreader.asm']
      projectsources += [path / 'x64-pngreader.asm']

      add_project_arguments('-DJPGR_CFG_EXTERNALASM', language: 'c')
      add_project_arguments('-DPNGR_CFG_EXTERNALASM', language: 'c')
    else
      warning('NASM not found, using C fallback code')
    endif
  endif
endif


# Source files
projectsources += [
  'src/version.c',
  'src/pngreader.c',
  'src/jpgreader.c',
]


headerfiles = [
  'jimage/pngreader.h',
  'jimage/pngreader.h',
]


common = {'sources': projectsources, 'dependencies': deps}

if meson.is_subproject()
  extraargs = []
  if get_option('default_library') == 'shared'
    lib = shared_library(meson.project_name(), kwargs: common, soversion: versionarray[0], c_args: ['-DJIMAGE_BUILDDLL', '-DJIMAGE_DLL'])
    if target_machine.system() in ['windows', 'cygwin']
      extraargs += ['-DJIMAGE_DLL']
    endif
  endif
  if get_option('default_library') == 'static'
    lib = static_library(meson.project_name(), kwargs: common, pic: true)
  endif

  if not is_variable('lib')
    error('default_library is not shared or static')
  endif
  jimage_dep = declare_dependency(
    link_with: lib, include_directories: include_directories('.'), dependencies: deps, compile_args: extraargs
  )
else
  jimage_shared = shared_library(meson.project_name(), kwargs: common, install: true, soversion: versionarray[0], c_args: ['-DJIMAGE_BUILDDLL', '-DJIMAGE_DLL'])
  jimage_static = static_library(meson.project_name(), kwargs: common, install: true, pic: true)

  jdeflate_dep = declare_dependency(
    link_with: [jimage_static, jimage_shared], include_directories: include_directories('.'), dependencies: deps
  )

  pkg = import('pkgconfig')
  pkg.generate(libraries: jdeflate_dep, version: meson.project_version(), name: meson.project_name(), filebase: meson.project_name(), description: 'Just another library')

  install_headers(headerfiles, preserve_path: true)
endif