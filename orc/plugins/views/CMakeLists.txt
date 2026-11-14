# In-tree view binaries: the tool windows of Decode-Orc's own stages, built
# and loaded exactly as a plugin's view binary is (plugin-architecture.md,
# "View Plugins"). Each links only the SDK headers, orc-view-support and Qt.
#
# Qt is found here, not borrowed: orc/gui's find_package targets are local to
# that directory, and whether Qt is also visible from orc/ depends on how
# QtNodes was found (system package yes, submodule no — macOS and Windows).
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_subdirectory(core)
