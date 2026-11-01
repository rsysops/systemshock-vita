Controls:
* Add in-game keybinds editor
* Cameras have only 1 axis on movement?
* Cyberspace controls are bad? Could add rear touchscreen controls as option?
* Gyro controls 2 axes sensitivity options
* Pass on the prefs.txt (all options)
* Refactor keybinds.txt to have the Vita inputs instead of PC keyboard
* Running
* Option: Enable touchscreen = None, Front, Rear, Both
* Cumulative jumps (like Mario), is this normal?
* The game should use the vita code directly, not translating to a virtual keyboard first

Documentation:
* Bump to 1.0
* Explain the changes from the parent project + screenshots in README

Features:
* FOV
* Modifications?
* Quick save?

Fixes:
* Audio mixing
* Crash when hibernated Vita?
* Saving: write to a temporary file, then rename it over the slot (the slot is emptied before the copy, so an interrupted save loses the old one too)
* Saving: block the PS button, quick menu and power menu during a save (sceShellUtilLock)
* Saving: tell the player when a save fails (save_game returns OK even if the copy fails)

Performance:
* FPS lock 30 option
* Improve performance (GPU) (ongoing)
* System Shock Enhanced shadow render as a mod

User interface:
* Options items labels to review
* Traduction "Guide"?
* Ajouter un tooltip pour les options?
* Display game version + hash in the main menu
