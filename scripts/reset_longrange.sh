#!/bin/bash
# Notes for returning a thermometer from Long Range to legacy advertising.
cat <<'TXT'
A thermometer in LE Long Range mode cannot be reached by the legacy-mode flasher or a BT 4.2 adapter.
Options (pvvx README, lines 138-144):
  1. Remove and re-insert the battery: the Long Range flag is not persisted across power loss
     (src/app.h: "сбрасывается после отключения питания"). BT5 PHY stays enabled (harmless).
  2. From a BT5-capable central (nRF Connect on a phone, or this project's Coded-PHY connection):
     write 0xDD (CMD_ID_LR_RESET) or 0x56 (defaults) to characteristic 0x1F1F.
After that the device advertises legacy again and scripts/set_longrange.sh can be used to re-enable LR.
TXT
