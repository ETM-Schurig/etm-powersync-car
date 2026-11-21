isEmpty(PLUGIN_PRI) {
    exists($$[QT_INSTALL_PREFIX]/include/nymea/plugin.pri) {
        include($$[QT_INSTALL_PREFIX]/include/nymea/plugin.pri)
    } else {
        error("plugin.pri not found: install libnymea-dev or pass PLUGIN_PRI=/path/to/plugin.pri")
    }
} else {
    include($$PLUGIN_PRI)
}
