#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. A shader that will not
#                 compile presents to an operator as "the effect does nothing".
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   --global      Global on returns the input bit-exactly, with a negative
#                 control that proves the check can fail.
#   --skew        a bar moving at v px/frame leans by v over a one-frame
#                 readout, for both interpolations and both vertical
#                 directions.
#   --band        a flash pulse lights ( E + L ) / T_read of the frame, where
#                 the phase says it should.
#   --flicker     50 Hz and 60 Hz mains bands sit T_light / T_read of the
#                 frame apart.
#   --jello       a sinusoidal shake wobbles the rows with the period and the
#                 amplitude the closed form gives.
#   --mirror      the OpenFX build's CPU readout -- a second copy of the
#                 readout shader, fed by a memoryless route to its uniforms --
#                 matches the GPU frame for frame, with a negative control.
#   sweep         does every control change the picture. A GLSL uniform whose
#                 name does not match the C++ is ignored without a word, and
#                 this is the only thing standing between a typo and a shipped
#                 slider that does nothing.
#   bench         the render cost, for the record. Not pass/fail.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#   openfx        the OpenFX bundle: its plist names the binary on disk, it is
#                 universal, exports OfxGetPlugin, ad-hoc signs, and ofxprobe
#                 loads it, sees the identity and controls it should, and
#                 renders -- leaving a still picture alone, as a rolling
#                 shutter must, and changing it once the mains light is on.
#
# The last six are release-job work done locally on purpose. A check that
# only runs in CI, after a tag, is a check that catches you after the tag.
#
set -uo pipefail

cd "$(dirname "$0")/.."

# resolume-ofx-bridge, for ofxprobe. It sits beside this repo's checkout --
# and from a git worktree `..` is the worktrees folder, not Projects/resolume,
# so the main checkout is found through git's common dir as well.
# READOUT_BRIDGE overrides both, and OFXPROBE the probe itself.
BRIDGE="${READOUT_BRIDGE:-}"
if [ -z "$BRIDGE" ]; then
	for candidate in "../resolume-ofx-bridge" \
	                 "$(dirname "$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null)")/../resolume-ofx-bridge"; do
		if [ -d "$candidate/build" ]; then
			BRIDGE="$candidate"
			break
		fi
	done
fi
BRIDGE="${BRIDGE:-../resolume-ofx-bridge}"

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails.
#---------------------------------------------------------------------------
shaders_compile() {
	local dir bad=0 n=0 shader

	if ! command -v glslc >/dev/null 2>&1; then
		printf '   skipped: glslc not installed (brew install shaderc)\n'
		return 0
	fi

	dir="$( mktemp -d )"

	python3 - "$dir" <<'SHADERS_PY'
import re, sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )

# Where this repo keeps its GLSL.
FILES = [
	"source/Shaders.cpp",
]

# A shader may be several adjacent raw strings (MSVC caps one literal at
# about 16 KB), so everything up to the terminating semicolon is joined.
named = {}
for f in FILES:
	text = pathlib.Path( f ).read_text()
	for m in re.finditer( r'(\w+)\s*=\s*((?:\s*(?://[^\n]*\n)*\s*R"\(.*?\)")+)\s*;', text, re.S ):
		named[ m.group( 1 ) ] = "".join( re.findall( r'R"\((.*?)\)"', m.group( 2 ), re.S ) )

def emit( name, body ):
	ext = ".vert" if re.search( r"\bgl_Position\s*=", body ) else ".frag"
	( out / ( name + ext ) ).write_text( body )

for name, body in named.items():
	if body.lstrip().startswith( "#version" ) and "void main" in body:
		emit( name, body )
SHADERS_PY

	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations \
			   "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done

	if [ "$n" -eq 0 ]; then
		# No shaders at all is a FAILURE, not a pass. It means the extraction
		# above has lost track of where this repo keeps its GLSL.
		printf '   no shaders were extracted -- the extraction has gone stale\n'
		rm -rf "$dir"
		return 1
	fi

	if [ "$bad" -eq 0 ]; then
		printf '   %d shaders, all compile\n' "$n"
	fi
	rm -rf "$dir"
	return "$bad"
}

step "shaders"
if shaders_compile; then
	pass "every shader compiles"
else
	fail "a shader does not compile"
fi

#---------------------------------------------------------------------------
# The browser demo carries a SECOND copy of all three shaders, because a page
# cannot include a C++ file. Two copies of a shader is exactly the arrangement
# that drifts, and the drift is invisible from both sides: the plugin keeps
# working, the page keeps working, and they quietly stop being the same effect.
# The page's whole claim is that what it runs is the plugin's own code.
#
# Character for character, whitespace included -- "it is only a reformat" is how
# a real change gets waved through.
#---------------------------------------------------------------------------
step "demo shaders"
if [ -f demo/tools/check_shaders.py ]; then
	if out=$(python3 demo/tools/check_shaders.py 2>&1); then
		pass "$( printf '%s\n' "$out" | tail -1 )"
	else
		fail "demo/plugin.js has drifted from source/Shaders.cpp"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
else
	fail "demo/tools/check_shaders.py is missing -- nothing is checking the demo's shader copies"
fi

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

ROTEST="$BUILD/rotest"

step "physics"
for check in global skew band flicker jello mirror; do
	if out=$("$ROTEST" --$check 2>&1); then
		pass "rotest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "rotest --$check"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

step "sweep"
if out=$(python3 tools/sweep.py --binary "$ROTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$ROTEST" --bench --frames 60 2>&1 | sed -n '3,7p' | sed 's/^/   /'

BUNDLE="$BUILD/Readout.bundle"
BIN="$BUNDLE/Contents/MacOS/Readout"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.readout" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Readout.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Readout" "id:          RO01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

#---------------------------------------------------------------------------
# The OpenFX bundle.
#
# cmake/InfoOFX.plist.in is one of the files copied from repo to repo, and the
# copy the fleet started from had the PREVIOUS plugin's name hardcoded into
# CFBundleExecutable. Nothing fails until the release job's codesign, after
# the tag, with a message that never mentions the plist. So the plist is
# checked against the binary and the exact codesign is run, on a copy.
#
# ofxprobe renders one frame at time 0 of a still ramp. A rolling shutter
# cannot see a still picture, so at the defaults the output MUST be the input
# -- and with the mains light on it must not be, which is what shows the
# render ran at all.
#---------------------------------------------------------------------------
OFX_BUNDLE="$BUILD/Readout.ofx.bundle"
OFX_ID="com.stoatworks.readout"

if [ "$(uname)" = "Darwin" ]; then
	step "openfx"
	if [ ! -d "$OFX_BUNDLE" ]; then
		fail "no OpenFX bundle at $OFX_BUNDLE -- configure without -DBUILD_OFX=OFF"
	else
		ofxExe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null)
		ofxIdent=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null)
		OFX_BIN="$OFX_BUNDLE/Contents/MacOS/$ofxExe"
		if [ -n "$ofxExe" ] && [ -f "$OFX_BIN" ]; then
			pass "CFBundleExecutable ($ofxExe) is on disk"
		else
			fail "CFBundleExecutable is '$ofxExe' but no such binary exists -- codesign will fail after the tag"
		fi
		if [ "$ofxIdent" = "$OFX_ID.ofx" ]; then
			pass "CFBundleIdentifier is $ofxIdent"
		else
			fail "CFBundleIdentifier is '$ofxIdent'"
		fi

		archs=$(lipo -archs "$OFX_BIN" 2>/dev/null)
		case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 in the OpenFX binary (got: $archs)" ;; esac
		case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 in the OpenFX binary (got: $archs)" ;; esac

		syms=$(nm -gU "$OFX_BIN" 2>/dev/null)
		case "$syms" in
			*_OfxGetPlugin*) pass "exports OfxGetPlugin" ;;
			*) fail "no OfxGetPlugin -- a host will find no plugin in the bundle" ;;
		esac

		tmp=$(mktemp -d)
		cp -R "$OFX_BUNDLE" "$tmp/" 2>/dev/null
		if codesign --force --sign - --timestamp=none "$tmp/Readout.ofx.bundle" >/dev/null 2>&1; then
			pass "ad-hoc signs (the command the release job runs)"
		else
			fail "the OpenFX bundle will not ad-hoc sign"
		fi
		rm -rf "$tmp"

		OFXPROBE="${OFXPROBE:-$BRIDGE/build/ofxprobe}"
		if [ -x "$OFXPROBE" ]; then
			# ofxprobe ADDS --dir to the standard scan, and the first bundle with a
			# matching identifier wins -- so an installed copy in /Library/OFX/Plugins
			# would be tested instead of this build. The manifest says which.
			manifest=$(mktemp)
			"$OFXPROBE" --dir "$BUILD" --json >"$manifest" 2>/dev/null
			if python3 - "$OFX_ID" "$OFX_BUNDLE" "$manifest" <<'PROBE_PY'
import json, os, sys
want, bundle = sys.argv[ 1 ], os.path.realpath( sys.argv[ 2 ] )
data = json.load( open( sys.argv[ 3 ] ) )
plugins = data if isinstance( data, list ) else data.get( "plugins", [ data ] )
mine = [ p for p in plugins if p.get( "identifier" ) == want ]
if not mine:
	sys.exit( "no plugin with identifier " + want )
p = mine[ 0 ]
if os.path.realpath( p.get( "bundlePath", "" ) ) != bundle:
	sys.exit( "the probe found " + want + " at " + p.get( "bundlePath", "?" ) + ", not in this build" )
names = { q[ "name" ] for q in p[ "params" ] }
missing = { "readoutTime", "exposure", "direction", "interpolation", "global", "amount", "frequency",
            "rotation", "trigger", "fireAt", "interval", "length", "phase", "level", "colour", "mains",
            "depth", "mix" } - names
audio = [ n for n in names if "audio" in n.lower() or n.lower() == "fire" ]
if p.get( "label" ) != "Readout" or p.get( "grouping" ) != "Stoatworks":
	sys.exit( "label/grouping is %r/%r" % ( p.get( "label" ), p.get( "grouping" ) ) )
if missing:
	sys.exit( "controls missing: " + ", ".join( sorted( missing ) ) )
if audio:
	sys.exit( "FFGL-only controls declared: " + ", ".join( sorted( audio ) ) )
PROBE_PY
			then
				pass "ofxprobe sees Readout / Stoatworks from this build, every control, and no audio"
			else
				fail "ofxprobe's manifest is wrong -- see: $OFXPROBE --dir $BUILD --json"
			fi
			rm -f "$manifest"

			still=$("$OFXPROBE" --dir "$BUILD" --render "$OFX_ID" --size 640x360 2>&1)
			lit=$("$OFXPROBE" --dir "$BUILD" --render "$OFX_ID" --size 640x360 --set mains=1 --set depth=0.8 2>&1)
			case "$still" in
				*" 0 of "*" bytes differ"*) pass "renders a still picture untouched at the defaults" ;;
				*) fail "the defaults changed a still picture"; printf '%s\n' "$still" | sed 's/^/      /' ;;
			esac
			changed=$(printf '%s\n' "$lit" | grep -oE '[0-9]+ of [0-9]+ bytes differ')
			case "$changed" in
				""|"0 of "*) fail "Mains on did not change the picture"; printf '%s\n' "$lit" | sed 's/^/      /' ;;
				*) pass "Mains on changes it ($changed)" ;;
			esac
		else
			printf '   skipped: ofxprobe not built at %s\n' "$OFXPROBE"
		fi

		#-------------------------------------------------------------------
		# Stricter than Resolve's Fusion page.
		#
		# Fusion gives an OpenFX plugin's clips no frame rate (the effect has
		# one). The first build read a clip's rate unguarded and every Fusion
		# render failed. A test host with `--quirks fusion` is stricter: it
		# withholds the effect's rate too and reports every clip's frame range
		# as [0, 0]. It is scratch tooling outside this repo, so the step runs
		# only where one is found -- $OFXHOST, or an ofxprobe whose --help lists
		# --quirks -- and skips otherwise.
		#
		# A bar moving across 12 frames, rendered at frame 10 with the longest
		# window: under the quirk it must render, ask for frames 7..10, equal the
		# normal host at 24 fps byte for byte (the fallback rate), and differ from
		# a Global render that reads only the current frame -- so the window still
		# reaches back past the [0, 0] range.
		#-------------------------------------------------------------------
		step "openfx under Fusion's quirks"
		QUIRKHOST=""
		for candidate in "${OFXHOST:-}" "$OFXPROBE"; do
			[ -n "$candidate" ] && [ -x "$candidate" ] || continue
			case "$("$candidate" --help 2>&1)" in
				*--quirks*) QUIRKHOST="$candidate"; break ;;
			esac
		done
		if [ -z "$QUIRKHOST" ]; then
			printf '   skipped: no test host with --quirks fusion (set OFXHOST to one)\n'
		else
			qdir=$(mktemp -d)
			python3 - "$qdir" <<'QUIRK_PY'
import sys, pathlib
out = pathlib.Path( sys.argv[ 1 ] )
w, h = 160, 90
for f in range( 12 ):
	rows = bytearray()
	for y in range( h ):
		for x in range( w ):
			on = 10 + 9 * f <= x < 16 + 9 * f
			rows += bytes( ( 240, 240, 240 ) if on else ( 60, 70, 110 ) )
	( out / ( "f%04d.ppm" % f ) ).write_bytes( b"P6\n%d %d\n255\n" % ( w, h ) + bytes( rows ) )
QUIRK_PY
			qbase=( --no-system-dirs --dir "$BUILD" --render "$OFX_ID" --seq "$qdir/f%04d.ppm" --time 10 )
			qwin=( --set readoutTime=1 --set exposure=1 )
			quirk=$("$QUIRKHOST" "${qbase[@]}" --quirks fusion --frames-needed "${qwin[@]}" --out-only "$qdir/q.ppm" 2>&1)
			qstatus=$?
			"$QUIRKHOST" "${qbase[@]}" --frame-rate 24 "${qwin[@]}" --out-only "$qdir/n.ppm" >/dev/null 2>&1
			"$QUIRKHOST" "${qbase[@]}" --quirks fusion --set global=1 --set exposure=0 --out-only "$qdir/g.ppm" >/dev/null 2>&1
			if [ "$qstatus" -ne 0 ] || [ ! -s "$qdir/q.ppm" ]; then
				fail "does not render under --quirks fusion -- the Fusion failure"
				printf '%s\n' "$quirk" | tail -5 | sed 's/^/      /'
			else
				pass "renders with no frame rate and a [0, 0] frame range"
				case "$quirk" in
					*"[7, 10]"*) pass "asks for frames 7..10 at the 24 fps fallback" ;;
					*) fail "frames needed under the quirk are not 7..10"; printf '%s\n' "$quirk" | grep -i needed | sed 's/^/      /' ;;
				esac
				if cmp -s "$qdir/q.ppm" "$qdir/n.ppm"; then
					pass "byte-identical to the normal host at 24 fps"
				else
					fail "differs from the normal host at 24 fps -- the fallback is not 24, or the window collapsed"
				fi
				if [ -s "$qdir/g.ppm" ] && ! cmp -s "$qdir/q.ppm" "$qdir/g.ppm"; then
					pass "reads earlier frames: differs from a render of the current frame alone"
				else
					fail "identical to a render of the current frame alone -- the window did not reach back"
				fi
			fi
			rm -rf "$qdir"
		fi
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
