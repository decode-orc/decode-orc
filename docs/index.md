# Home

Welcome to the decode-orc documentation. Here you will find user and technical information about decode-orc.

![](assets/decode-orc_logotype-1024x286.png)

# What is decode-orc?

decode-orc is both a graphical (GUI) environment and a CLI tool for the orchestration of complex video decoding tasks.  It allows the user to control the overall flow and effect of the decoding process using intuitive tools that provide powerful configuration and control whilst also being repeatable and deterministic.

![](assets/orc-gui-screenshot.png)

# Where to start

* To install decode-orc, see [Installation](installation/linux-flatpak.md) (Linux, macOS and Windows packages, or building from source).
* New to decode-orc? Read the [GUI overview](gui-user-guide/overview.md) for the core ideas, then create a [quick project](gui-user-guide/quick-projects.md) from one of your captures.
* The [main window](gui-user-guide/dialogues/main.md) and [preview](gui-user-guide/dialogues/preview.md) pages describe the GUI; the [stage reference](gui-user-guide/stages/overview.md) describes every stage and its parameters.
* Scripting or batch work? See the [Orc-CLI user guide](cli-user-guide/overview.md).
* Want stages that do not ship with decode-orc? See [Using Plugins](plugins.md) to find, install and update third-party plugins.

Note that there are differences between the older versions of ld-decode and vhs-decode and the ld-decode-tool chain which can cause minor incompatibility issues with decode-orc.  If your dual-source TBCs don't have the right file extensions or you get warnings about legacy JSON metadata - please see the [section on migration from legacy tools](misc/migration.md).

# Development status

decode-orc is a new replacement for the ld-decode-tools, so it is relatively new.  You might find issues and even bugs.  You can help out by reporting those issues to the project or even by assisting with the development of decode-orc.  You will find information about how to do this (as well as information about how to contact the developers) under the miscellaneous section of the documentation.

# Further information

This documentation aims to provide both user-level and technical-level documentation. If you spot a mistake and would like to report a documentation issue or contribute documentation updates, please use the main decode-orc repository:

[decode-orc GitHub repository](https://github.com/decode-orc/decode-orc){target="_blank"}

decode-orc is not a standalone application - it is designed to be used with ld-decode and the (ever expanding) decoder variations such as VHS Decode.  You can find information about ld-decode from the project's documentation site:

[ld-decode documentation](https://happycube.github.io/ld-decode-docs/){target="_blank"}

