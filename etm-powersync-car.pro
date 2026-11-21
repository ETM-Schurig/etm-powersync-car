TEMPLATE = subdirs

# One directory per vehicle maker, as in nymea-plugins.
PLUGIN_DIRS = \
    skoda       \

message(============================================)
message("Qt version:" $$[QT_VERSION])

SUBDIRS += $$PLUGIN_DIRS

# Tests are built only with CONFIG+=build_tests (CI and local development),
# not by the package build.
build_tests {
    SUBDIRS += tests
}
