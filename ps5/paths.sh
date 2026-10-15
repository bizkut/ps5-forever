# Where the port lives on the console: inside the launcher title's folder,
# /data/homebrew/$TITLE_ID, which ShadowMountPlus mounts as the title's
# /app0. Wine is in its wine/ subfolder: the install (bin, lib, share), the
# prefix (prefix), HOME (home) and test programs (tests). Nothing goes
# elsewhere in /data/homebrew, which holds other homebrew.
#
# The title folder must be exactly /data/homebrew/$TITLE_ID: the path is built
# into Wine (the Unix modules' sonames are absolute). It also keeps Wine on
# the internal drive's case-sensitive file system (external drives are
# usually exFAT). Games installed by Windows programs may go on any drive:
# Wine's z: shows /data and /mnt.
#
# A change here needs ps5/build-wine.sh configure, a full build, a FreeType
# rebuild and a stage. Read by ps5/build-wine.sh, ps5/build-freetype.sh,
# ps5/stage-wine.sh and ps5/deploy-wine.py.
TITLE_ID=PPSA99030
WINE_ROOT=/data/homebrew/$TITLE_ID/wine
