# Génère une image flash complète (bootloader + partitions + app) pour Wokwi,
# sinon la partition de données (LittleFS) n'existe pas dans la simulation.
Import("env")

APP_BIN = "$BUILD_DIR/${PROGNAME}.bin"
MERGED_BIN = "$BUILD_DIR/${PROGNAME}_merged.bin"
board = env.BoardConfig()


def merge_bin(source, target, env):
    images = env.Flatten(env.get("FLASH_EXTRA_IMAGES", [])) + ["$ESP32_APP_OFFSET", APP_BIN]
    cmd = ["$PYTHONEXE", "$OBJCOPY", "--chip", board.get("build.mcu", "esp32"), "merge_bin",
           "--fill-flash-size", board.get("upload.flash_size", "4MB"), "-o", MERGED_BIN] + images
    env.Execute(" ".join(cmd))


env.AddPostAction(APP_BIN, merge_bin)
