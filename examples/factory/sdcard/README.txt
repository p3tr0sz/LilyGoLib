T-Watch SD card — seed content

This folder is the version-controlled "source of truth" for the non-desktop
files that live on the watch's SD card: network-radio presets, Evil Portal
templates, a place for music, and so on. It is mirrored onto a card by:

    tools/web_build.py --sync <card-root>

--sync copies these files onto the card *only when they are not already there*,
so it never clobbers anything the watch or you have saved. Pass --force to
overwrite the seed files with the repo's copies. It also lays down the web
desktop (web/) and creates the empty data/ capture folders in the same pass.

Layout (relative to the card root):

  radio_streams.txt              network-radio station presets (Name|URL per line)
  data/music/                    drop your own .mp3 files here (scanned from the
                                 whole card, but this keeps them tidy)
  data/evilportal/portals/       custom captive-portal .html templates
  data/ir/ data/gnss/ data/nfc/  where the watch's tools save captures
  data/recordings/ data/lora/
  data/sniffer/ data/link/
