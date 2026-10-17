# Mouse fix for Worms Ultimate Mayhem (1077)

Mouse behaves insanely wonky in this game and this patch should make it work smooth like butter.

### What it does and why?

In Worms Ultimate Mayhem mouse always moves as if the screen has 640x480 resolution, besides that, when you move cursor it always gets stuck, jitters or jumps backwards.

- This patch makes it, so 1 mouse step = 1 screen pixel (just like it would behave outside of the game), because of that now mouse can feel very slow in game (3 times slower on 1920x1080). If you have muscle memory from unpatched game i recommend putting high mouse sensitivity in game settings. If that's not enough you can change speed in mousefix.ini file. 

- Menu cursor was drawn every 20 ms, with patch it's drawn every frame. Inside of the match cursor snaps mouse back to the screen centre every frame.

- Because Windows' mouse messages get delivered to the game with slight delay, sometimes outdated reports make the game confused and mouse moves to opposite direction from where you were moving it. Patch snaps mouse itself, right before that it reads where mouse really was, during matches makes the game ignore Windows' delayed messages entirely.

Check mousefix.ini file for additional settings.

### How to install it?

Download the zip file from [releases](https://github.com/emiwum/WUMMouseFix/releases/tag/WUM.Mouse.Fix), put contents of the zip file inside of the game's main folder (WormsXHD). Voila

### How to UNINSTALL it?

Delete mousefix.asi and mousefix.ini from WormsXHD\plugins. Duh

### Does it work online? Does it cause desyncs?

Most likely not. As much as I tested it didn't cause any desyncs and honestly I will be surprised if it will.

## Check out the video comparison of the game with and without the patch:

[![Mouse fix showcase](https://img.youtube.com/vi/rehD9dvHr4c/maxresdefault.jpg)](https://www.youtube.com/watch?v=rehD9dvHr4c)


Includes [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)

Developed with assistance from [Claude](https://claude.com/)
