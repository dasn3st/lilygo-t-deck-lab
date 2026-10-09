# Refuse to finish a build whose PSRAM driver does not match the board.
#
# WHY THIS EXISTS: the colour T-Deck has OCTAL PSRAM and the T-Deck Max has QUAD, but a
# single machine keeps ONE compiled copy of the Espressif framework libraries. Whichever
# board built last leaves its PSRAM driver behind in that copy. The platform's own guard
# against this is a hash written into <project>/sdkconfig.defaults - it records what THIS
# PROJECT last asked for, not what the shared libraries actually contain. With the two
# boards checked out in two folders there are two such hashes, each honestly reporting a
# match, and nothing compares either of them to the libraries. So a board switch could
# silently link the other board's driver. That shipped once, on 2026-08-26, and the Max
# boot-looped: "octal_psram: PSRAM chip is not connected, or wrong PSRAM line mode".
#
# The driver name survives into the firmware image as a literal string, so the finished
# binary can be asked directly which driver it got. That is what this does, and it is
# checked against the board's own declaration rather than a remembered rule:
#
#   needs QUAD  <- the env's custom_sdkconfig says CONFIG_SPIRAM_MODE_QUAD=y
#   needs OCTAL <- the board's memory_type is an OPI ("..._opi") type
#   neither     <- board has no PSRAM to get wrong; nothing to check
#
# A mismatch aborts the build, so a wrong image cannot reach the flashing step at all.
Import("env")

import sys
from os.path import basename


def _expected_driver(env):
    sdkcfg = env.GetProjectOption("custom_sdkconfig", "") or ""
    if "CONFIG_SPIRAM_MODE_QUAD=y" in sdkcfg.replace(" ", ""):
        return "quad"
    mem = (env.BoardConfig().get("build.arduino.memory_type", "") or "")
    if mem.endswith("_opi") or mem.startswith("opi_"):
        return "octal"
    return None


def check_psram_driver(source, target, env):
    expected = _expected_driver(env)
    if not expected:
        return

    path = str(target[0])
    try:
        with open(path, "rb") as fh:
            blob = fh.read()
    except IOError as exc:
        sys.stderr.write("PSRAM driver check: cannot read %s (%s)\n" % (path, exc))
        env.Exit(1)
        return

    found = [name for name in ("octal", "quad") if (name + "_psram").encode() in blob]

    if found == [expected]:
        print("PSRAM driver check: %s carries the %s driver - correct for this board."
              % (basename(path), expected))
        return

    sys.stderr.write("\n")
    sys.stderr.write("*** PSRAM DRIVER MISMATCH - BUILD STOPPED ***\n")
    sys.stderr.write("  binary        : %s\n" % path)
    sys.stderr.write("  this board needs: %s\n" % expected)
    sys.stderr.write("  binary contains : %s\n" % (", ".join(found) if found else "no PSRAM driver at all"))
    sys.stderr.write("\n")
    sys.stderr.write("  Flashing this would boot-loop the device. The compiled Espressif\n")
    sys.stderr.write("  libraries almost certainly belong to the OTHER board. To force them to\n")
    sys.stderr.write("  be rebuilt for this one, blank the hash on line 1 of sdkconfig.defaults\n")
    sys.stderr.write("  and build again - the log must then show 'Reinstall Arduino framework'\n")
    sys.stderr.write("  and 'Compile Arduino IDF libs':\n")
    sys.stderr.write("\n")
    sys.stderr.write("    python -c \"p='sdkconfig.defaults';d=open(p,'rb').read();n=d.find(b'\\n');\"\n")
    sys.stderr.write("    \"open(p,'wb').write(b'# TASMOTA__0000000000000000'+d[n:])\"\n")
    sys.stderr.write("\n")
    env.Exit(1)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_psram_driver)
