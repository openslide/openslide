min_python = '3.10'

mkenv = files('mkenv.py')
requirements_txt = files('../requirements.txt')

pymod = import('python')
python = pymod.find_installation()
if python.language_version().version_compare('<' + min_python)
  # EL 8 and 9 platform Python is too old
  foreach pyver : [20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10]
    python = pymod.find_installation(
      'python3.' + pyver.to_string(),
      required : false,
    )
    if python.found()
      break
    endif
  endforeach
  if not python.found()
    error('Python >= ' + min_python + ' not found')
  endif
endif

if host_machine.system() == 'windows'
  subdir('Scripts')
else
  subdir('bin')
endif
