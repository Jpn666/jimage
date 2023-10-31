project('deflate', 'c', default_options: ['c_std=c99', 'buildtype=release', 'warning_level=3'], version: run_command('python', join_paths(meson.current_source_dir(), 'tools/version.py'), check: true).stdout().strip())


deps = []
deps += [dependency('', fallback: ['ctoolbox', 'lib'])]
deps += [dependency('', fallback: ['deflate', 'lib'])]


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


target = target_machine.cpu_family()
externalobjs = []
externalasm  = false


if target == 'x86_64'
  supported = false

  path = 'src/asm/'
  if target_machine.system() == 'unix'
    path = path / 'unix'
    supported = true
  endif
  if target_machine.system() == 'windows'
    path = path / 'windows'
    supported = true
  endif

  if supported
    externalobjs += [path / 'pngreader-' + target + '.o']
    externalobjs += [path / 'jpgreader-' + target + '.o']
    add_project_arguments('-DJPGR_CFG_EXTERNALASM', language: 'c')
    add_project_arguments('-DPNGR_CFG_EXTERNALASM', language: 'c')
    externalasm = true
  endif
endif


projectsources = [
  'src/pngreader.c', 'src/jpgreader.c'
]

largs = []
if externalasm
  cc = meson.get_compiler('c')
  if cc.get_linker_id() == 'ld.bfd'
    largs += ['-z', 'noexecstack']
  endif
endif


static_build = static_library('image', sources: projectsources, pic: true, install: true,  dependencies: deps)

lib = declare_dependency(
  link_with: static_build, include_directories: include_directories('.'), dependencies: deps, link_args: largs
)
