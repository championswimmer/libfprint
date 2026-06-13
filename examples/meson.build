
examples = [
    'enroll',
    'eh577-enroll-helper',
    'eh577-identify-helper',
    'eh577-verify-helper',
    'eh577-capture-helper',
    'identify',
    'img-capture',
    'manage-prints',
    'verify',
    'clear-storage',
]

foreach example: examples
    executable(example,
        [ example + '.c', 'storage.c', 'utilities.c' ],
        dependencies: [
            libfprint_dep,
            glib_dep,
        ],
    )
endforeach

executable('cpp-test',
    'cpp-test.cpp',
    dependencies: libfprint_dep,
)

# Offline minutiae counter: needs the internal fpi-image.h to build an FpImage
# from a PGM, so it gets an extra include dir into the libfprint source.
executable('eh577-pgm-minutiae',
    'eh577-pgm-minutiae.c',
    dependencies: [ libfprint_dep, glib_dep ],
    include_directories: include_directories('../libfprint'),
)

# All-pairs bozorth3 score matrix: needs internal headers and NBIS private symbols.
# The NBIS functions (bozorth3, lfs2nist_minutia_XYT, sort_x_y) are not exported
# from libfprint-2.so, so we also link against the internal static library.
executable('eh577-pgm-match',
    'eh577-pgm-match.c',
    dependencies: [ libfprint_dep, glib_dep ],
    include_directories: include_directories('../libfprint'),
    link_with: libfprint_private,
)

executable('eh577-pgm-match-relaxed',
    [ 'eh577-pgm-match-relaxed.c', 'eh577-relaxed-bozorth.c' ],
    dependencies: [ libfprint_dep, glib_dep ],
    include_directories: [
        include_directories('../libfprint'),
        include_directories('../libfprint/nbis/include'),
    ],
    c_args: [
        '-Wno-error=redundant-decls',
        '-Wno-array-parameter',
    ],
    link_with: libfprint_private,
)

if installed_tests
    install_subdir('prints',
        install_dir: installed_tests_testdir)
endif
