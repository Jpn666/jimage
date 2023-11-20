project('jimage', 'c', default_options: ['c_std=c99', 'buildtype=release', 'warning_level=3'], version: run_command('python', join_paths(meson.current_source_dir(), 'tools/version.py'), check: true).stdout().strip())


deps = []
deps += [dependency('', fallback: ['ctoolbox', 'lib'])]
deps += [dependency('', fallback: ['jdeflate', 'lib'])]

cc = meson.get_compiler('c')
if cc.get_id() == 'msvc'
  add_project_arguments('-D__MSVC__', language: 'c')
endif


# major, minor and path
versionarray = [0, 0, 0]

mmp = meson.project_version().split('.')
if mmp.length() == 3
  versionarray = [
    mmp[0].to_int(),
    mmp[1].to_int(),
    mmp[2].to_int()
  ]
endif


# Configuration
conf = configuration_data()

# build the version string
v0 = versionarray[0]
v1 = versionarray[1]
v2 = versionarray[2]
version = f'@v0@.@v1@.@v2@'


conf.set('version', version)

conf.set('version_major', v0)
conf.set('version_minor', v1)
conf.set('version_rpath', v2)

projectsources = []


isunix    = false
iswindows = false

# System platform
if target_machine.system() == 'windows'
  iswindows = true
else
  if target_machine.system() == 'haiku'
    pass
  else
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


python = find_program('python3')
script = join_paths(meson.current_source_dir(), 'tools/listfiles.py')
newline = '''
'''


base = 'src'
r = run_command(python, script, base, '.c', check: true).stdout()
foreach file: r.split(newline)
  if not (file == '')
    projectsources += [file]
  endif
endforeach


headerfiles = []

base = 'jimage'
r = run_command(python, script, base, '.h', check: true).stdout()
foreach file: r.split(newline)
  if not (file == '')
    headerfiles += [file]
  endif
endforeach

configfile = configure_file(input: 'jimageconfig.h.in', output: 'jimageconfig.h', configuration: conf)
headerfiles += [configfile]


largs = []

static_build = static_library('jimage', sources: projectsources, pic: true, install: true, dependencies: deps)
lib = declare_dependency(
  link_with: static_build, include_directories: include_directories('.'), dependencies: deps, link_args: largs
)

install_headers(headerfiles, preserve_path: true)
