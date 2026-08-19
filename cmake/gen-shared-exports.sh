#!/bin/sh
# Generate the PE module-definition file that makes liblogos_core.dll the single
# provider of the shared C++ runtime (TokenManager, LogosAPI, LogosAPIClient and
# the LogosResult stream operators).
#
# WHY a generated .def rather than __declspec(dllexport) in the headers: the
# definitions live in liblogos_protocol.a / liblogos_qt_host.a (the Qt half was
# liblogos_qt_sdk.a until the host runtime moved into logos-qt-host; same
# objects, new home), which are also
# linked by logos_host.exe, ui-host.exe, every module plugin and every native
# platform. Annotating them for export would mean a second, Windows-only,
# export-annotated build of both archives, kept in sync forever, to solve a
# Windows-only problem. Exporting at liblogos_core's link instead leaves those
# archives compiled byte-identically to before; the consumer side is a pure
# opt-in (-DLOGOS_SHARED_USE_DLL, see logos-protocol/cpp/logos_shared_api.h).
#
# WHY the whole archive and not a curated class list: ld chooses archive members
# by object file, for reasons that have nothing to do with our symbols. Measured
# on this build, main_ui referenced std::string's move constructor and ld
# satisfied it out of the Qt archive's logos_api.cpp.obj — which then dragged
# LogosAPI, LogosAPIClient and TokenManager in behind it. The consumers
# therefore link an EMPTY archive (logos-basecamp/cmake/LogosSharedFromDll.cmake)
# and take everything from the DLL, which only works if the DLL really does
# provide everything. Hence --whole-archive at the link and "*" here: a partial
# export set turns into an undefined reference in a downstream repo, far from
# the cause.
#
# WHY COMDAT symbols are filtered out: they come from inline functions and
# templates in headers, so every consumer TU emits its own copy regardless of
# what the DLL exports. Exporting them turns the import library into a STRONG
# definition that then collides with the consumer's own copy. They are also
# precisely the symbols an export cannot deduplicate — a function-local static
# inside an inline function stays per-image on PE no matter what. Do not
# "simplify" this filter away.
#
# Usage: gen-shared-exports.sh <nm> <out.def> <archive> <obj,obj,...|*> [...]

set -eu

NM="$1"; shift
OUT="$1"; shift

TMP="${OUT}.tmp"
: > "$TMP"

while [ "$#" -gt 0 ]; do
    archive="$1"; members="$2"; shift 2
    [ -f "$archive" ] || { echo "gen-shared-exports: missing archive $archive" >&2; exit 1; }
    "$NM" -A --defined-only "$archive" | awk -v members="$members" -v arch="$archive" '
    BEGIN {
        if (members == "*") { all = 1 }
        else { n = split(members, a, ","); for (i = 1; i <= n; i++) want[a[i]] = 1 }
    }
    {
        # nm -A on an archive prints "<archive>:<member>:<addr> <type> <name>".
        split($1, p, ":")
        mem = p[2]; typ = $2; name = $3

        # COMDAT sections are named ".text$<mangled>" / ".rdata$<mangled>" /
        # etc. Record the mangled tail so the symbol itself can be dropped.
        if (name ~ /^\.[a-z]+\$/) {
            tail = name; sub(/^\.[a-z]+\$/, "", tail)
            comdat[mem SUBSEP tail] = 1
            next
        }
        if (!all && !(mem in want)) next
        # T=text D=data R=rodata B=bss, all uppercase == external linkage.
        # Weak (V/W) is COMDAT by another name; lowercase is file-local and
        # cannot be exported at all (that includes the function-local statics
        # themselves — we export the ACCESSOR so callers reach ours).
        if (typ != "T" && typ != "D" && typ != "R" && typ != "B") next
        # Itanium-mangled C++ (^_Z), PLUS the logos-protocol C ABI (^lp_).
        #
        # The ^_Z test alone keeps toolchain bookkeeping such as
        # qt_version_tag_6_11_used — which every image legitimately defines —
        # out of the export table. But `lp_*` is `extern "C"`, so it is
        # UNMANGLED and the same test silently dropped the entire C ABI.
        #
        # That was invisible until B5: before it, only C++ callers reached the
        # shared runtime. B5 re-emits every Qt-typed dependency wrapper as a
        # VENEER over the lp path, so a consumer that compiles such a wrapper —
        # logos-basecamp compiles package_manager_api.cpp into its own exe —
        # now calls lp_invoke / lp_client_create / lp_token_save directly. With
        # them absent from the .def the exe cannot import them, and the link
        # fails with plain `undefined reference to 'lp_invoke'`.
        #
        # Exporting is the correct fix rather than letting the consumer link
        # liblogos_protocol.a itself: lp_token_save and friends operate on the
        # TokenManager singleton, so a static copy in the exe would reinstate
        # exactly the split-brain token store this whole .def scheme exists to
        # prevent. Module plugins are separate processes and keep their own
        # per-image copy by design (see logos_module_grant_host_services).
        #
        # The prefix is deliberately tight: `lp_` only, not "anything unmangled".
        if (name !~ /^_Z/ && name !~ /^lp_/) next
        seenmem[mem] = 1
        k++; recmem[k] = mem; rectyp[k] = typ; recnam[k] = name
    }
    END {
        if (!all) for (m in want) if (!(m in seenmem)) {
            printf("gen-shared-exports: %s has no member %s\n", arch, m) > "/dev/stderr"
            bad = 1
        }
        if (bad) exit 1
        for (i = 1; i <= k; i++) {
            if ((recmem[i] SUBSEP recnam[i]) in comdat) continue
            print recnam[i] (rectyp[i] == "T" ? "" : " DATA")
        }
    }' >> "$TMP"
done

# DATA matters: a data export reached without the DATA keyword hands the
# consumer the CONTENTS of the slot instead of its address, which corrupts at
# runtime rather than at link. The keyword is derived from nm's type letter
# above, never written by hand.
{
    echo "EXPORTS"
    sort -u "$TMP"
} > "$OUT"
rm -f "$TMP"

count=$(grep -c . "$OUT" || true)
echo "gen-shared-exports: wrote $OUT ($((count - 1)) symbols)"
