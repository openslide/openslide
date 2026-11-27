test_python = custom_target(
  'python.exe',
  command : [python, mkenv, '@INPUT@', '@OUTPUT@'],
  input : requirements_txt,
  output : 'python.exe',
  build_by_default : true,
)
