# Generated by devtools/yamaker.

PY3_PROGRAM()

WITHOUT_LICENSE_TEXTS()

VERSION(16.0.0)

LICENSE(NCSA)

PEERDIR(
    contrib/libs/clang16/bindings/python
)

NO_LINT()

SRCDIR(contrib/libs/clang16/bindings/python/examples)

PY_SRCS(
    MAIN cindex/cindex-dump.py
)

END()
