# Section: CLI build configuration
# Defines the orc-cli executable and associated build settings
add_executable(orc-cli 
    orc_cli.cpp
    command_process.cpp
    command_plugins.cpp
    command_stages.cpp
    command_nodes.cpp
    command_tools.cpp
    analysis_report_json.cpp
    project_args.cpp
    json_writer.cpp
    plugin_json.cpp
    stage_json.cpp
)

# Section: Architecture enforcement (MVP)
# CLI uses presenter and common layers; prevents direct core linkage
target_link_libraries(orc-cli PRIVATE 
    orc-presenters
    orc-common
)

# Section: Public include paths for CLI
target_include_directories(orc-cli PRIVATE
    ${CMAKE_SOURCE_DIR}/orc/presenters/include
    ${CMAKE_SOURCE_DIR}/orc/public
    ${CMAKE_SOURCE_DIR}/orc/common/include
    ${CMAKE_BINARY_DIR}/generated
)

# Section: Build-time flags for MVP enforcement
target_compile_definitions(orc-cli PRIVATE
    ORC_CLI_BUILD
)

if(APPLE)
    set_target_properties(orc-cli PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
elseif(UNIX AND NOT WIN32)
    set_target_properties(orc-cli PROPERTIES INSTALL_RPATH "\$ORIGIN/../lib;\$ORIGIN/../lib/orc-stage-plugins")
endif()

# Run with UTF-8 as the process code page so non-ASCII paths given on the
# command line reach the filesystem intact (no-op off Windows).
orc_apply_utf8_manifest(orc-cli)

# Section: Install rules
if(APPLE)
    # Embed CLI inside the macOS app bundle so drag-and-drop install contains both tools
    install(TARGETS orc-cli
        RUNTIME DESTINATION "orc-gui.app/Contents/MacOS"
        COMPONENT Runtime
    )
else()
    install(TARGETS orc-cli
        RUNTIME DESTINATION bin
        COMPONENT Runtime
    )
endif()

if(BUILD_UNIT_TESTS)
    add_test(
        NAME CLI.HelpShowsProcessOnlyCommand
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsProcessOnlyCommand PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--process"
    )

    add_test(
        NAME CLI.HelpShowsPluginsSubcommand
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsPluginsSubcommand PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "plugins"
    )

    add_test(
        NAME CLI.HelpShowsSafeCorePluginsOption
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsSafeCorePluginsOption PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--safe-core-plugins"
    )

    add_test(
        NAME CLI.PluginsListWithSafeCorePluginsBeforeSubcommandSucceeds
        COMMAND $<TARGET_FILE:orc-cli> plugins --safe-core-plugins list
    )
    set_tests_properties(CLI.PluginsListWithSafeCorePluginsBeforeSubcommandSucceeds PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Registry path:"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-safe-before;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-safe-before"
    )

    add_test(
        NAME CLI.PluginsListWithSafeCorePluginsAfterSubcommandSucceeds
        COMMAND $<TARGET_FILE:orc-cli> plugins list --safe-core-plugins
    )
    set_tests_properties(CLI.PluginsListWithSafeCorePluginsAfterSubcommandSucceeds PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Registry path:"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-safe-after;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-safe-after"
    )

    add_test(
        NAME CLI.PluginsListSucceeds
        COMMAND $<TARGET_FILE:orc-cli> plugins list
    )
    set_tests_properties(CLI.PluginsListSucceeds PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Registry path:"
    )

    # Placeholder text is never printed where a selector belongs.
    add_test(
        NAME CLI.PluginsListPrintsNoPlaceholderIds
        COMMAND $<TARGET_FILE:orc-cli> plugins list --core
    )
    set_tests_properties(CLI.PluginsListPrintsNoPlaceholderIds PROPERTIES
        LABELS "unit;cli"
        FAIL_REGULAR_EXPRESSION "<unnamed>"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-list-core;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-list-core"
    )

    # The default listing is local-only: with every route to the network
    # pointed at a closed port it must still succeed, and promptly.
    add_test(
        NAME CLI.PluginsListMakesNoNetworkRequest
        COMMAND $<TARGET_FILE:orc-cli> plugins list
    )
    set_tests_properties(CLI.PluginsListMakesNoNetworkRequest PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Registry path:"
        TIMEOUT 30
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-list-offline;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-list-offline;http_proxy=http://127.0.0.1:9;https_proxy=http://127.0.0.1:9;all_proxy=http://127.0.0.1:9"
    )

    add_test(
        NAME CLI.PluginsHelpDocumentsCorePluginsHiddenByDefault
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpDocumentsCorePluginsHiddenByDefault PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "core plugins are hidden"
    )

    add_test(
        NAME CLI.PluginsAddMissingPathFails
        COMMAND $<TARGET_FILE:orc-cli> plugins add
    )
    set_tests_properties(CLI.PluginsAddMissingPathFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    # 'add' with neither form must name both of them.
    add_test(
        NAME CLI.PluginsAddWithNoSourceNamesBothForms
        COMMAND $<TARGET_FILE:orc-cli> plugins add
    )
    set_tests_properties(CLI.PluginsAddWithNoSourceNamesBothForms PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--url <releases-url>"
    )

    add_test(
        NAME CLI.PluginsAddRejectsPathAndUrlTogether
        COMMAND $<TARGET_FILE:orc-cli> plugins add /nonexistent/plugin.so
                --url https://github.com/example/plugin/releases --yes
    )
    set_tests_properties(CLI.PluginsAddRejectsPathAndUrlTogether PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsAddUrlRejectsLocalOnlyOptions
        COMMAND $<TARGET_FILE:orc-cli> plugins add
                --url https://github.com/example/plugin/releases
                --id com.example.plugin --yes
    )
    set_tests_properties(CLI.PluginsAddUrlRejectsLocalOnlyOptions PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsHelpShowsAddUrlForm
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpShowsAddUrlForm PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "add --url <releases-url>"
    )

    # Trust is confirmed before any command lets a plugin binary run, and no
    # command blocks when there is no terminal to confirm on.
    add_test(
        NAME CLI.PluginTrustConfirmation
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_plugin_trust_prompt.sh $<TARGET_FILE:orc-cli>
    )
    set_tests_properties(CLI.PluginTrustConfirmation PROPERTIES
        LABELS "unit;cli"
        TIMEOUT 120
    )

    add_test(
        NAME CLI.PluginsRemoveMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins remove
    )
    set_tests_properties(CLI.PluginsRemoveMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsRemoveUnknownSelectorFails
        COMMAND $<TARGET_FILE:orc-cli> plugins remove path:/nonexistent/plugin.so
    )
    set_tests_properties(CLI.PluginsRemoveUnknownSelectorFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-remove-selector;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-remove-selector"
    )

    add_test(
        NAME CLI.PluginsHelpDocumentsSelectors
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpDocumentsSelectors PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "remove \\[--dry-run\\] <selector>"
    )

    # Every identifier the CLI prints must be accepted verbatim by the commands
    # that take one: plugin selectors (including for an entry with no plugin
    # id) and stage names.
    add_test(
        NAME CLI.PluginSelectorRoundTrip
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_plugin_selector_roundtrip.sh $<TARGET_FILE:orc-cli>
    )
    set_tests_properties(CLI.PluginSelectorRoundTrip PROPERTIES
        LABELS "unit;cli"
        TIMEOUT 180
    )

    # Neither front end may grow or lose a plugin/stage capability the other
    # has without the capability manifest recording both sides.
    add_test(
        NAME CLI.PluginUxCapabilityParity
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_plugin_ux_parity.sh $<TARGET_FILE:orc-cli> ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(CLI.PluginUxCapabilityParity PROPERTIES
        LABELS "unit;cli"
    )

    # `nodes list / get / set` against real project files in a temporary
    # directory, hence functional rather than unit.
    add_test(
        NAME CLI.NodesCommands
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_cli_nodes.sh $<TARGET_FILE:orc-cli> ${CMAKE_SOURCE_DIR}/orc-tests/fixtures/cli-projects
    )
    set_tests_properties(CLI.NodesCommands PROPERTIES
        LABELS "functional;cli"
        TIMEOUT 120
    )

    # `tools list / run`: listing and every refusal against real project
    # files. Set ORC_CLI_TOOLS_TBC to a composite PAL or NTSC .tbc to add a
    # Disc Mapper run on it; without one that part is skipped.
    add_test(
        NAME CLI.ToolsCommands
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_cli_tools.sh $<TARGET_FILE:orc-cli> ${CMAKE_SOURCE_DIR}/orc-tests/fixtures/cli-projects
    )
    set_tests_properties(CLI.ToolsCommands PROPERTIES
        LABELS "functional;cli"
        TIMEOUT 600
    )

    add_test(
        NAME CLI.PluginsEnableMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins enable
    )
    set_tests_properties(CLI.PluginsEnableMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsDisableMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins disable
    )
    set_tests_properties(CLI.PluginsDisableMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    # 'search' with no term lists the whole index, like the GUI Browse dialog
    # opening on a full list. The index is seeded as a last-good cache and the
    # fetch is pointed at a closed port, so the test needs no network; an index
    # with no entries also resolves no upstream releases.
    # Written here rather than shipped as a fixture file: `test-data/` is
    # gitignored, and an index with no entries is two lines.
    file(WRITE
        ${CMAKE_CURRENT_BINARY_DIR}/test-home-search/decode-orc/plugin-index-cache.yaml
        "registry_schema: 2\nplugins: []\n"
    )
    add_test(
        NAME CLI.PluginsSearchWithNoTermSucceeds
        COMMAND $<TARGET_FILE:orc-cli> plugins search
    )
    set_tests_properties(CLI.PluginsSearchWithNoTermSucceeds PROPERTIES
        LABELS "unit;cli"
        TIMEOUT 30
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-search;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-search;APPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-home-search;ORC_PLUGIN_INDEX_URL=http://127.0.0.1:9/index.yaml"
    )

    add_test(
        NAME CLI.PluginsSearchRejectsContradictoryFilters
        COMMAND $<TARGET_FILE:orc-cli> plugins search --installed --available
    )
    set_tests_properties(CLI.PluginsSearchRejectsContradictoryFilters PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsHelpShowsSearchFilters
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpShowsSearchFilters PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--compatible"
    )

    add_test(
        NAME CLI.PluginsUpdateRejectsSelectorWithAll
        COMMAND $<TARGET_FILE:orc-cli> plugins update com.example.plugin --all --yes
    )
    set_tests_properties(CLI.PluginsUpdateRejectsSelectorWithAll PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsUpdateWithNoTargetFails
        COMMAND $<TARGET_FILE:orc-cli> plugins update
    )
    set_tests_properties(CLI.PluginsUpdateWithNoTargetFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsHelpShowsUpdateAll
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpShowsUpdateAll PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "update <selector> \\| --all"
    )

    add_test(
        NAME CLI.PluginsInfoMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins info
    )
    set_tests_properties(CLI.PluginsInfoMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    # 'info' also describes an installed plugin, so an id in neither place must
    # say that both were searched. The index is seeded as an empty last-good
    # cache and its fetch pointed at a closed port, so the test needs no
    # network yet still exercises the index-was-consulted path.
    file(WRITE
        ${CMAKE_CURRENT_BINARY_DIR}/test-home-info/decode-orc/plugin-index-cache.yaml
        "registry_schema: 2\nplugins: []\n"
    )
    add_test(
        NAME CLI.PluginsInfoUnknownIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins info com.example.absent
    )
    set_tests_properties(CLI.PluginsInfoUnknownIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
        TIMEOUT 30
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-info;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-info;APPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-home-info;ORC_PLUGIN_INDEX_URL=http://127.0.0.1:9/index.yaml"
    )

    add_test(
        NAME CLI.PluginsInfoUnknownIdNamesBothPlacesSearched
        COMMAND $<TARGET_FILE:orc-cli> plugins info com.example.absent
    )
    set_tests_properties(CLI.PluginsInfoUnknownIdNamesBothPlacesSearched PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "installed registry or in the available plugin index"
        TIMEOUT 30
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-info;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-info;APPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-home-info;ORC_PLUGIN_INDEX_URL=http://127.0.0.1:9/index.yaml"
    )

    # 'doctor' reports; a problem it finds is the answer, not a failure.
    add_test(
        NAME CLI.PluginsDoctorSucceedsOnACleanRegistry
        COMMAND $<TARGET_FILE:orc-cli> plugins doctor
    )
    set_tests_properties(CLI.PluginsDoctorSucceedsOnACleanRegistry PROPERTIES
        LABELS "unit;cli"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-doctor;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-doctor"
    )

    add_test(
        NAME CLI.PluginsDoctorReportsSearchPaths
        COMMAND $<TARGET_FILE:orc-cli> plugins doctor
    )
    set_tests_properties(CLI.PluginsDoctorReportsSearchPaths PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Plugin search paths"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-doctor;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-doctor"
    )

    add_test(
        NAME CLI.PluginsHelpShowsDoctor
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpShowsDoctor PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "doctor"
    )

    add_test(
        NAME CLI.PluginsInstallMissingIdFails
        COMMAND $<TARGET_FILE:orc-cli> plugins install
    )
    set_tests_properties(CLI.PluginsInstallMissingIdFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.PluginsHelpShowsDiscoverySubcommands
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpShowsDiscoverySubcommands PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "search"
    )

    add_test(
        NAME CLI.PluginsUnknownSubcommandFails
        COMMAND $<TARGET_FILE:orc-cli> plugins frobnicate
    )
    set_tests_properties(CLI.PluginsUnknownSubcommandFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    # --- Scripting contract ---------------------------------------------------

    # Every --json document parses, keys nothing as a display label, and carries
    # identifiers the commands that take one accept unchanged.
    add_test(
        NAME CLI.JsonContract
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_cli_json_contract.sh $<TARGET_FILE:orc-cli>
    )
    set_tests_properties(CLI.JsonContract PROPERTIES
        LABELS "unit;cli"
        TIMEOUT 180
    )

    # Every documented exit code is returned by the path that owns it, and no
    # command blocks when stdin is not a terminal.
    add_test(
        NAME CLI.ExitCodeContract
        COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/cmake/check_cli_exit_codes.sh $<TARGET_FILE:orc-cli>
    )
    set_tests_properties(CLI.ExitCodeContract PROPERTIES
        LABELS "unit;cli"
        TIMEOUT 180
    )

    # The listing modes are two projections of one structure, so --json says
    # what the table says; these pin the shape a script keys on.
    add_test(
        NAME CLI.PluginsListJsonCarriesSelectors
        COMMAND $<TARGET_FILE:orc-cli> plugins list --core --json
    )
    set_tests_properties(CLI.PluginsListJsonCarriesSelectors PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "\"registry_path\""
        FAIL_REGULAR_EXPRESSION "<unnamed>"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-json;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-json"
    )

    # Enum values are the presenter's stable ids, never the words the table
    # prints for the same state.
    add_test(
        NAME CLI.StagesListJsonUsesStableEnumIds
        COMMAND $<TARGET_FILE:orc-cli> stages list --core --kind source --json
    )
    set_tests_properties(CLI.StagesListJsonUsesStableEnumIds PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "\"kind\": \"source\""
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-json;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-json"
    )

    add_test(
        NAME CLI.PluginsHelpDocumentsJson
        COMMAND $<TARGET_FILE:orc-cli> plugins --help
    )
    set_tests_properties(CLI.PluginsHelpDocumentsJson PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--json"
    )

    add_test(
        NAME CLI.StagesHelpDocumentsJson
        COMMAND $<TARGET_FILE:orc-cli> stages --help
    )
    set_tests_properties(CLI.StagesHelpDocumentsJson PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--json"
    )

    # --- Stage introspection --------------------------------------------------

    add_test(
        NAME CLI.HelpShowsStagesSubcommand
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpShowsStagesSubcommand PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "stages list"
    )

    add_test(
        NAME CLI.StagesHelpShowsSubcommands
        COMMAND $<TARGET_FILE:orc-cli> stages --help
    )
    set_tests_properties(CLI.StagesHelpShowsSubcommands PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "info <stage>"
    )

    add_test(
        NAME CLI.StagesHelpDocumentsListFilters
        COMMAND $<TARGET_FILE:orc-cli> stages --help
    )
    set_tests_properties(CLI.StagesHelpDocumentsListFilters PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "--kind KIND"
    )

    # A core stage this build always registers must be listed, and the listing
    # must work in safe mode (core plugins only).
    add_test(
        NAME CLI.StagesListShowsACoreStage
        COMMAND $<TARGET_FILE:orc-cli> stages --safe-core-plugins list --core
    )
    set_tests_properties(CLI.StagesListShowsACoreStage PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "name: +tbc_source"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages"
    )

    # The name column carries the internal stage name (what a project's
    # `stage:` key holds), never a display name.
    add_test(
        NAME CLI.StagesListNamesAreStageIds
        COMMAND $<TARGET_FILE:orc-cli> stages list --core --kind source
    )
    set_tests_properties(CLI.StagesListNamesAreStageIds PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "name: +[A-Za-z0-9_]+\n"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages"
    )

    # The owning plugin id a stage reports is a plugin selector: the two
    # commands take the same string, for a bundled plugin as for any other.
    file(WRITE
        ${CMAKE_CURRENT_BINARY_DIR}/test-home-stages/decode-orc/plugin-index-cache.yaml
        "registry_schema: 2\nplugins: []\n"
    )
    add_test(
        NAME CLI.StagesListPluginIdIsAPluginSelector
        COMMAND $<TARGET_FILE:orc-cli> plugins info decode-orc.stage.tbc_source
    )
    set_tests_properties(CLI.StagesListPluginIdIsAPluginSelector PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "selector: +decode-orc.stage.tbc_source"
        TIMEOUT 30
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;APPDATA=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;ORC_PLUGIN_INDEX_URL=http://127.0.0.1:9/index.yaml"
    )

    add_test(
        NAME CLI.StagesListFiltersByOwningPlugin
        COMMAND $<TARGET_FILE:orc-cli> stages list --core
                --plugin decode-orc.stage.tbc_source
    )
    set_tests_properties(CLI.StagesListFiltersByOwningPlugin PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Stages \\(1\\)"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages"
    )

    add_test(
        NAME CLI.StagesListRejectsUnknownKind
        COMMAND $<TARGET_FILE:orc-cli> stages list --kind decoder
    )
    set_tests_properties(CLI.StagesListRejectsUnknownKind PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    # `transform` is the one word for a processing stage.
    add_test(
        NAME CLI.StagesListRejectsFilterKind
        COMMAND $<TARGET_FILE:orc-cli> stages list --kind filter
    )
    set_tests_properties(CLI.StagesListRejectsFilterKind PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.StagesInfoReportsParametersOfACoreStage
        COMMAND $<TARGET_FILE:orc-cli> stages info tbc_source
    )
    set_tests_properties(CLI.StagesInfoReportsParametersOfACoreStage PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Parameters \\("
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages"
    )

    # An unknown stage is a not-found, and it offers the names it does have.
    add_test(
        NAME CLI.StagesInfoUnknownStageFails
        COMMAND $<TARGET_FILE:orc-cli> stages info tbc_sourc
    )
    set_tests_properties(CLI.StagesInfoUnknownStageFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages"
    )

    add_test(
        NAME CLI.StagesInfoUnknownStageListsNearMatches
        COMMAND $<TARGET_FILE:orc-cli> stages info tbc_sourc
    )
    set_tests_properties(CLI.StagesInfoUnknownStageListsNearMatches PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "Did you mean:"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages"
    )

    add_test(
        NAME CLI.StagesInfoRejectsYaml
        COMMAND $<TARGET_FILE:orc-cli> stages info tbc_source --yaml
    )
    set_tests_properties(CLI.StagesInfoRejectsYaml PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    # A core stage ships instructions.md beside its binary; 'help' prints it.
    add_test(
        NAME CLI.StagesHelpPrintsCoreStageInstructions
        COMMAND $<TARGET_FILE:orc-cli> stages help tbc_source
    )
    # Pin the stage's own heading, not just any Markdown character, so a wrong
    # or empty document cannot pass.
    set_tests_properties(CLI.StagesHelpPrintsCoreStageInstructions PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "# TBC Source"
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages"
    )

    add_test(
        NAME CLI.StagesHelpUnknownStageFails
        COMMAND $<TARGET_FILE:orc-cli> stages help definitely_not_a_stage
    )
    set_tests_properties(CLI.StagesHelpUnknownStageFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages;HOME=${CMAKE_CURRENT_BINARY_DIR}/test-home-stages"
    )

    add_test(
        NAME CLI.StagesUnknownSubcommandFails
        COMMAND $<TARGET_FILE:orc-cli> stages frobnicate
    )
    set_tests_properties(CLI.StagesUnknownSubcommandFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    add_test(
        NAME CLI.RejectsPreviewExportOption
        COMMAND $<TARGET_FILE:orc-cli> --preview-export
    )
    set_tests_properties(CLI.RejectsPreviewExportOption PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )

    # A project that loads but has nothing to trigger fails, and says why (not
    # because the file is missing).
    add_test(
        NAME CLI.ProcessNoSinksFails
        COMMAND $<TARGET_FILE:orc-cli>
                ${CMAKE_SOURCE_DIR}/orc-tests/fixtures/cli-projects/no_sinks.orcprj
                --process
    )
    set_tests_properties(CLI.ProcessNoSinksFails PROPERTIES
        LABELS "unit;cli"
        WILL_FAIL TRUE
    )
    add_test(
        NAME CLI.ProcessNoSinksSaysThereIsNoSink
        COMMAND $<TARGET_FILE:orc-cli>
                ${CMAKE_SOURCE_DIR}/orc-tests/fixtures/cli-projects/no_sinks.orcprj
                --process
    )
    set_tests_properties(CLI.ProcessNoSinksSaysThereIsNoSink PROPERTIES
        LABELS "unit;cli"
        PASS_REGULAR_EXPRESSION "No triggerable sink nodes"
    )

    # --- Removed command-line pipeline options ---------------------------------

    # The removed options are refused with a pointer to the migration notes,
    # not as an unknown option. check_cli_exit_codes.sh pins their exit code.
    foreach(removed_option --source -i --filters -f --sink -o --export-project
            --video-format --source-type)
        add_test(
            NAME CLI.RemovedOption${removed_option}PointsToMigration
            COMMAND $<TARGET_FILE:orc-cli> ${removed_option} value
        )
        set_tests_properties(CLI.RemovedOption${removed_option}PointsToMigration
            PROPERTIES
            LABELS "unit;cli"
            PASS_REGULAR_EXPRESSION "migration notes"
        )
    endforeach()

    add_test(
        NAME CLI.HelpListsNoRemovedOptions
        COMMAND $<TARGET_FILE:orc-cli> --help
    )
    set_tests_properties(CLI.HelpListsNoRemovedOptions PROPERTIES
        LABELS "unit;cli"
        FAIL_REGULAR_EXPRESSION "--source|--filters|--sink|--export-project|--video-format|--source-type"
    )

    find_package(GTest REQUIRED)

    # Pure unit test for the JSON emitter: escaping and document structure, with
    # no presenter and no registry involved.
    add_executable(orc-cli-json-writer-test
        json_writer_test.cpp
        json_writer.cpp
    )
    target_include_directories(orc-cli-json-writer-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(orc-cli-json-writer-test PRIVATE
        GTest::gtest_main
        spdlog::spdlog
    )
    add_test(
        NAME CLI.JsonWriterUnit
        COMMAND $<TARGET_FILE:orc-cli-json-writer-test>
    )
    set_tests_properties(CLI.JsonWriterUnit PROPERTIES
        LABELS "unit;cli"
    )

    # Pure unit test for the JSON form of analysis items: no presenter and no
    # registry involved.
    add_executable(orc-cli-analysis-report-json-test
        analysis_report_json_test.cpp
        analysis_report_json.cpp
        json_writer.cpp
    )
    target_include_directories(orc-cli-analysis-report-json-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/orc/view-types
        ${CMAKE_SOURCE_DIR}/orc/sdk/include
    )
    target_link_libraries(orc-cli-analysis-report-json-test PRIVATE
        GTest::gtest_main
        spdlog::spdlog
    )
    add_test(
        NAME CLI.AnalysisReportJsonUnit
        COMMAND $<TARGET_FILE:orc-cli-analysis-report-json-test>
    )
    set_tests_properties(CLI.AnalysisReportJsonUnit PROPERTIES
        LABELS "unit;cli"
    )
endif()
