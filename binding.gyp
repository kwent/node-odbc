{
  'targets' : [
    {
      'target_name' : 'odbc_bindings',
      'sources' : [ 
        'src/odbc.cpp',
        'src/odbc_connection.cpp',
        'src/odbc_statement.cpp',
        'src/odbc_result.cpp',
        'src/dynodbc.cpp'
      ],
      'cflags' : ['-Wall', '-Wextra', '-Wno-unused-parameter'],
      'include_dirs': [
        "<!(node -e \"require('nan')\")"
      ],
      'defines' : [
        'UNICODE'
      ],
      'conditions' : [
        [ 'OS == "linux"', {
          'libraries' : [
            '/usr/lib/x86_64-linux-gnu/libltdl.a',
            '/usr/lib/x86_64-linux-gnu/libodbc.a',
            '/usr/lib/x86_64-linux-gnu/libodbccr.a',
            '/usr/lib/x86_64-linux-gnu/libodbcinst.a'
          ],
          'cflags' : [
            '-g'
          ]
        }],
        [ 'OS == "mac"', {
          'include_dirs': [
            '/usr/local/include'
          ],
          'libraries' : [ 
            '-L/usr/lib -liconv',
            '/usr/local/lib/libltdl.a',
            '/usr/local/lib/libodbc.a',
            '/usr/local/lib/libodbccr.a',
            '/usr/local/lib/libodbcinst.a'
          ]
        }],
        [ 'OS=="win"', {
          'sources' : [
            'src/strptime.c',
            'src/odbc.cpp'
          ],
          'libraries' : [ 
            'odbccp32.lib'
          ]
        }]
      ]
    }
  ]
}
