HD texture packs (for example an HD HUD)
=========================================

The game can load higher-resolution versions of its own textures from this folder.
This uses PCSX2's texture replacement, so packs made for PCSX2 (Shinobi USA, SLUS-20459) work here too.

Installing a pack
-----------------
Put the pack's .png files in:

    textures\SLUS-20459\replacements\

and make sure "HD texture packs" is on in the launcher's Mod Menu (it is by default). Restart the game.

Making a pack
-------------
1. In the launcher's Mod Menu, switch on "Save textures (pack makers)" and play through the parts you want.
   Every texture the game uses is saved to textures\SLUS-20459\dumps\ (the game runs slower while this is on).
2. Switch it off again.
3. Pick the textures you want to improve (the HUD: health bar, magic, TATE counter, boss gauges, fonts...),
   make larger versions of them (2x or 4x the size, same proportions), and keep each file's name exactly as it was.
4. Put the new files in textures\SLUS-20459\replacements\ and restart the game.

Tip: PS2 textures store transparency as 0..128, which normal upscalers get wrong (bright fringes, dark halos).
scripts/ps2_upscale.py in the GitHub repository handles that for you with Real-ESRGAN (realesrgan-ncnn-vulkan): put it
and Real-ESRGAN in textures\SLUS-20459 as described at the top of the script and run it. In game, Shift+F5 switches
HD textures on and off, so you can compare.

Upscaling the original textures with an upscaling tool is fine; please don't replace them with AI-generated art.
