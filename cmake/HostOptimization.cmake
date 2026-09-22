# Host-tuned code generation for builds that stay on the machine that made them.
#
# ORC_NATIVE_ARCH  Compile for this machine's CPU (-march=native, or
#                  -mcpu=native where that is the spelling). The auto-vectoriser
#                  can then use the widest vector unit the CPU has (AVX2 or
#                  AVX-512 on current x86, instead of the SSE2 baseline that
#                  every x86-64 binary must otherwise assume) and the scheduler
#                  its pipeline. The result runs only on CPUs with the same
#                  instruction sets, so this is for local builds only.
#
# ORC_ENABLE_LTO   Link-time optimisation for Release configurations. Lets the
#                  compiler inline and specialise across translation units,
#                  which per-file -O3 cannot. Debug builds are left alone so
#                  the edit-build-test loop stays fast.
#
# Both default to ON for a local build and OFF under CI and packaging. CI is
# recognised by the CI environment variable that GitHub Actions and most other
# services export. The release presets in CMakePresets.json, the Nix package
# and the Flatpak manifest pass OFF explicitly as well: a shipped binary must
# run on any machine, and a reproducible build must not depend on the CPU
# that produced it.

if(DEFINED ENV{CI})
    set(_orc_host_opt_default OFF)
else()
    set(_orc_host_opt_default ON)
endif()

option(ORC_NATIVE_ARCH
    "Compile for this machine's CPU (-march=native); local builds only"
    ${_orc_host_opt_default})
option(ORC_ENABLE_LTO
    "Link-time optimisation for Release builds"
    ${_orc_host_opt_default})

if(ORC_NATIVE_ARCH)
    if(MSVC)
        # MSVC cannot query the build machine; /arch:AVX2 etc. would have to
        # be chosen by hand, which is not what this option promises.
        message(STATUS "Host optimisation: ORC_NATIVE_ARCH has no effect with MSVC")
    else()
        include(CheckCXXCompilerFlag)

        # x86 and most GCC targets spell it -march=native; AArch64 GCC and
        # Apple clang spell it -mcpu=native.
        check_cxx_compiler_flag("-march=native" ORC_HAVE_MARCH_NATIVE)
        if(ORC_HAVE_MARCH_NATIVE)
            set(_orc_native_flag "-march=native")
        else()
            check_cxx_compiler_flag("-mcpu=native" ORC_HAVE_MCPU_NATIVE)
            if(ORC_HAVE_MCPU_NATIVE)
                set(_orc_native_flag "-mcpu=native")
            endif()
        endif()

        if(_orc_native_flag)
            add_compile_options(${_orc_native_flag})
            message(STATUS "Host optimisation: ${_orc_native_flag}")
        else()
            message(WARNING
                "ORC_NATIVE_ARCH is ON but the compiler accepts neither "
                "-march=native nor -mcpu=native; building for the baseline ISA")
        endif()

        # The Nix compiler wrapper deletes -march=native from the command line
        # (silently, so the check above still passes) unless told not to. The
        # flake's default dev shell and its native package set this to 0; a
        # shell that does not will build for the baseline ISA regardless.
        if("$ENV{NIX_ENFORCE_NO_NATIVE}" STREQUAL "1")
            message(WARNING
                "NIX_ENFORCE_NO_NATIVE=1: the Nix compiler wrapper will drop "
                "${_orc_native_flag}; export NIX_ENFORCE_NO_NATIVE=0 to build "
                "for this machine")
        endif()

        # orc-core and the stage plugins are shared objects. By default GCC and
        # Clang assume any exported function may be replaced at load time
        # (LD_PRELOAD), so a call from one exported function to another in the
        # same library goes through the PLT and is never inlined. Nothing here
        # relies on interposition, so let the compiler see through those calls.
        check_cxx_compiler_flag("-fno-semantic-interposition"
            ORC_HAVE_NO_SEMANTIC_INTERPOSITION)
        if(ORC_HAVE_NO_SEMANTIC_INTERPOSITION)
            add_compile_options(-fno-semantic-interposition)
        endif()

        # Two pieces of -ffast-math that do not change any result. Without
        # -fno-math-errno every sqrt() and similar is a libm call that must
        # set errno, so it cannot be inlined to one instruction or vectorised;
        # nothing here reads errno after a maths call. -fno-trapping-math lets
        # the compiler assume floating-point exceptions are not trapped, which
        # is the default on every platform this runs on. The rest of
        # -ffast-math (reassociation, reciprocal approximations, finite-only,
        # flush-to-zero) alters output and breaks std::isnan, so it stays off.
        foreach(flag IN ITEMS -fno-math-errno -fno-trapping-math)
            string(MAKE_C_IDENTIFIER "ORC_HAVE_${flag}" _orc_flag_var)
            check_cxx_compiler_flag("${flag}" ${_orc_flag_var})
            if(${_orc_flag_var})
                add_compile_options(${flag})
            endif()
        endforeach()
    endif()
endif()

if(ORC_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _orc_ipo_supported OUTPUT _orc_ipo_reason LANGUAGES CXX)
    if(_orc_ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        message(STATUS "Host optimisation: link-time optimisation on for Release")
    else()
        message(STATUS
            "Host optimisation: link-time optimisation unavailable (${_orc_ipo_reason})")
    endif()
endif()
