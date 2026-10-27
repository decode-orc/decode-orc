# Shared plugin-side static libraries used by more than one stage plugin.
# These are NOT part of the SDK contract: they compile against the public
# SDK headers only and are linked privately into the plugins that need them.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 Simon Inns

add_subdirectory(audio-resample)
add_subdirectory(disc-metadata)
add_subdirectory(efm-decode)
add_subdirectory(vbi-services)
