test_python = custom_target(
  'python',
  command : [python, mkenv, '@INPUT@', '@OUTPUT@'],
  input : requirements_txt,
  output : 'python',
  build_by_default : true,
)
