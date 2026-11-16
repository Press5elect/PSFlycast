#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
"""PSFlyCast - every setting of the new interface, in one table.

The table below is the single place where a setting gets its area, section, level, label and
description. Where Flycast has the setting, its label and description are Flycast's own, word for
word (source F), or Flycast's own lines joined (f); a setting of PSFlyCast's own is written the way
Flycast writes them (P): a short sentence that starts with what it does.

  settings-table.py bake       writes shell/ps5/ui/ui_settings_table.inc (the C++ table)
  settings-table.py review     writes shell/ps5/docs/dev/settings-words.md (every word, for review)
  settings-table.py check      fails when a setting of the old interface has no place here, or the
                               .inc on disk is not what bake writes

Levels: E Essentials, M More, X Everything. Flags: g a game can have its own value; G only a game
has it (its page, never the Settings); r takes effect when PSFlyCast starts again; n at the next game.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
INC = ROOT / "shell/ps5/ui/ui_settings_table.inc"
REVIEW = ROOT / "shell/ps5/docs/dev/settings-words.md"
OLD = ROOT / "shell/ps5/docs/dev/settings-before.txt"

AREAS = []      # (name, blurb, [(section, [rows])])


def area(name, blurb):
    AREAS.append((name, blurb, []))


def section(name):
    AREAS[-1][2].append((name, []))


def row(id, kind, level, label, desc, src="P", was=None, choices=None, flags="", demo=0, unit="", lo=0, hi=0, step=1):
    AREAS[-1][2][-1][1].append(dict(id=id, kind=kind, level=level, label=label, desc=desc, src=src, was=was or label,
                                    choices=choices or [], flags=flags, demo=demo, unit=unit, lo=lo, hi=hi, step=step))


ON = None  # toggles have no choices

# ================================================================================ Picture
area("Picture", "How games are drawn and shown")
section("Resolution and Upscaling")
row("rend.Resolution", "choice", "E", "Internal Resolution",
    "Internal render resolution. Higher is better, but more demanding on the GPU. Values higher than your display resolution "
    "(but no more than double your display resolution) can be used for supersampling, which provides high-quality "
    "antialiasing without reducing sharpness.", "F", was="Internal resolution",
    choices=["Native (480p)", "2x (960p)", "3x (1440p)", "4x (1920p)", "4.5x (2160p)", "5x (2400p)", "6x (2880p)", "7x (3360p)",
             "8x (3840p)", "9x (4320p)", "10x (4800p)"], flags="g", demo=4)
row("ps5.upscaling", "choice", "E", "Upscaling",
    "Upscale the final picture to the screen. FSR 1, NIS and CAS sharpen it; Sharp Bilinear keeps pixels crisp; the CRT "
    "filters imitate a television.", was="Upscaling",
    choices=["Off", "FSR 1 Sharp", "FSR 1 Soft", "NIS", "CAS", "Sharp Bilinear", "Scanlines", "CRT"], flags="g", demo=1)
row("ps5.sharpening", "range", "M", "Sharpening", "Strength of NIS and CAS sharpening.", was="Sharpening",
    flags="g", demo=50, unit="%", lo=0, hi=100, step=10)
row("ps5.downsampling", "choice", "M", "Down-Sampling",
    "How a picture rendered larger than the screen is reduced. Soft averages every pixel, Sharp uses a Lanczos filter.",
    was="Down-sampling", choices=["Bilinear", "Soft", "Sharp"], demo=0)
row("ps5.smaa", "toggle", "M", "SMAA", "Smooth jagged edges with SMAA 1x. Costs a little GPU time.", was="SMAA", demo=1)
row("rend.AnisotropicFiltering", "choice", "M", "Anisotropic Filtering",
    "Higher values make textures viewed at oblique angles look sharper, but are more demanding on the GPU. This option only "
    "has a visible impact on mipmapped textures.", "F", was="Anisotropic filtering", choices=["Off", "2x", "4x", "8x", "16x"],
    flags="g", demo=4)
row("rend.IntegerScale", "toggle", "X", "Integer Scaling",
    "Scales the output by the maximum integer multiple allowed by the display resolution.", "F", was="Integer scaling", flags="g")
row("rend.LinearInterpolation", "toggle", "X", "Linear Interpolation",
    "Scales the output with linear interpolation. Will use nearest neighbor interpolation otherwise. Disable with integer "
    "scaling.", "F", was="Smooth scaling", flags="g", demo=1)

section("Screen and Aspect")
row("rend.WideScreen", "toggle", "E", "Widescreen",
    "Draw geometry outside of the normal 4:3 aspect ratio. May produce graphical glitches in the revealed areas.", "F",
    was="Widescreen", flags="g")
row("rend.WidescreenGameHacks", "toggle", "M", "Widescreen Game Cheats",
    "Modify the game so that it displays in 16:9 anamorphic format and use horizontal screen stretching. Only some games are "
    "supported.", "F", was="Widescreen game patches", flags="g")
row("rend.SuperWidescreen", "toggle", "X", "Super Widescreen",
    "Use the full width of the screen or window when its aspect ratio is greater than 16:9.", "F", was="Super widescreen", flags="g")
row("rend.StretchToFill", "toggle", "M", "Stretch to Fill", "Stretch the screen to fill the entire screen or window.", "F",
    was="Stretch to fill", flags="g")
row("rend.ScreenStretching", "range", "X", "Horizontal Stretching", "Stretch the screen horizontally", "F",
    was="Horizontal stretching", flags="g", demo=100, unit="%", lo=100, hi=250, step=5)
row("rend.Rotate90", "toggle", "M", "Rotate Screen 90°", "Rotate the screen 90° counterclockwise", "F",
    was="Rotate screen 90 degrees", flags="g")
row("ps5.border", "choice", "M", "Beside the Picture",
    "Fill the bars beside a 4:3 picture: black, a soft light taken from the picture, a gradient, a picture of your own, or the "
    "picture carried on.", was="Beside the picture",
    choices=["Black", "The Picture's Light", "Gradient", "Your Own Picture", "Carried On"], flags="g", demo=1)

section("Textures")
row("rend.TextureFiltering", "choice", "M", "Texture Filtering",
    "Default: Use the game's default texture filtering. Force Nearest-Neighbor: Crisper appearance, but may cause various "
    "rendering issues. Force Linear: Smoother appearance, but may cause various rendering issues.", "f",
    was="Texture filtering", choices=["Default", "Force Nearest-Neighbor", "Force Linear"], flags="g", demo=2)
row("rend.Mipmaps", "toggle", "X", "Mipmaps",
    "Use smaller versions of textures when they appear farther away. Disable for a sharper but noisier picture.", was="Mipmaps",
    flags="g", demo=1)
row("rend.TextureUpscale", "choice", "X", "Texture Upscaling",
    "Upscale textures with the xBRZ algorithm. Only on fast platforms and for certain 2D games", "F", was="Texture upscaling",
    choices=["Off", "2x", "4x", "6x"], flags="g")
row("rend.MaxFilteredTextureSize", "choice", "X", "Texture Max Size",
    "Textures larger than this dimension squared will not be upscaled", "F", was="Upscale textures up to",
    choices=["64", "128", "256", "512", "1024"], flags="g", demo=2)
row("rend.CustomTextures", "toggle", "M", "Load Custom Textures",
    "Load custom/high-res textures from data/textures/<game id>.", "F", was="Custom textures", flags="g")
row("rend.DumpTextures", "toggle", "X", "Dump Textures", "Dump all textures into data/texdump/<game id>", "F",
    was="Texture dump", flags="g")
row("ps5.texdumpPlace", "choice", "X", "Dump Textures To",
    "Where dumped textures are saved, in a folder for each game.", was="Dump textures to",
    choices=["Console Storage", "USB Drive"], flags="g")
row("rend.DumpUniqueTextures", "toggle", "X", "Discard Video and Animated Textures",
    "Skip dumping video (YUV) and already updated textures", "F", was="Skip changing textures", flags="g")
row("ps5.texdumpStatus", "info", "X", "Textures Saved", "Textures dumped while a game runs with Dump Textures enabled.",
    was="Textures saved")

section("Original Look")
row("ps5.originalLook", "choice", "M", "Original Look",
    "Turn on every option that makes the picture look as it did on the console: its texture filtering and mipmaps, 16-bit "
    "colour and the flicker filter.", was="Original look", choices=["Off", "On"], flags="g")
row("rend.AccurateMipmaps", "toggle", "X", "Accurate Mipmaps",
    "Choose mipmap levels the way the Dreamcast does. When disabled, levels are chosen for a sharper picture.",
    was="Accurate mipmaps", flags="g")
row("ps5.color16", "toggle", "X", "Original 16-Bit Colour",
    "Render 16-bit colour with the console's dithering. When disabled, colours are smooth.", was="Original 16-bit colour", flags="g")
row("ps5.flicker", "toggle", "X", "Flicker Filter",
    "Apply the console's vertical softening in TV modes. When disabled, lines are sharper.", was="Flicker filter", flags="g")
row("rend.Fog", "toggle", "X", "Fog", "Enable fog effects", "F", was="Fog", flags="g", demo=1)
row("rend.ModifierVolumes", "toggle", "X", "Shadows", "Enable modifier volumes, usually used for shadows", "F",
    was="Shadows (modifier volumes)", flags="g", demo=1)
row("ps5.depthMask", "toggle", "X", "Translucent Depth Mask",
    "Fix translucent polygons drawn behind objects they should be in front of, in a few games.", was="Translucent depth mask", flags="g")

section("Colour and Film Grain")
row("ps5.brightness", "range", "M", "Brightness", "Adjust the picture brightness. 100% is the original.", was="Brightness",
    flags="g", demo=100, unit="%", lo=50, hi=150, step=5)
row("ps5.contrast", "range", "M", "Contrast", "Adjust the picture contrast. 100% is the original.", was="Contrast",
    flags="g", demo=100, unit="%", lo=50, hi=150, step=5)
row("ps5.saturation", "range", "M", "Colour", "Adjust colour saturation. 0% is black and white; 100% is the original.",
    was="Colour", flags="g", demo=100, unit="%", lo=0, hi=200, step=10)
row("ps5.gamma", "range", "M", "Gamma", "Adjust gamma. Higher values brighten the shadows; 100% is the original.", was="Gamma",
    flags="g", demo=100, unit="%", lo=50, hi=150, step=5)
row("ps5.filmGrain", "range", "M", "Film Grain", "Add a fine, moving film grain over the picture, strongest in the mid-tones.",
    was="Film grain", flags="g", demo=0, unit="%", lo=0, hi=100, step=10)

section("HDR")
row("ps5.hdr", "toggle", "E", "HDR",
    "Output HDR10 to an HDR TV. Requires HDR enabled in the console's settings.", was="HDR")
row("ps5.hdrPeak", "choice", "M", "HDR Highlights",
    "Peak brightness of the game's brightest parts, in nits. Adjust HDR Brightness finds your TV's.", was="HDR highlights",
    choices=["Off", "400", "600", "800", "1000", "1500", "2000", "4000"], demo=4)
row("ps5.hdrWhite", "choice", "M", "HDR White Level", "Brightness of white, in nits. 203 is HDR's reference white.",
    was="HDR white level", choices=["100", "150", "203", "250", "300"], demo=2)
row("ps5.hdrAdjust", "action", "M", "Adjust HDR Brightness", "Show a test screen to find your TV's peak and white level.",
    was="Adjust HDR brightness")

section("Frame Rate")
row("ps5.framePacing", "choice", "M", "Frame Pacing",
    "Synchronizes the frame rate with the screen refresh rate. Sync to Display is the smoothest.", "f", was="Frame pacing",
    choices=["Off", "VSync", "Sync to Display"], flags="g", demo=2)
row("ps5.hz120", "toggle", "M", "120 Hz Output", "Output 4K at 120 Hz on a TV that supports it.", was="120 Hz output", flags="r", demo=1)
row("ps5.vrr", "toggle", "M", "Variable Refresh Rate",
    "Show each frame as soon as it is ready, on a VRR display with 120 Hz output. Experimental.", was="Variable refresh rate",
    flags="r", demo=1)
row("ps5.framegen", "choice", "M", "Frame Generation",
    "Generate frames in between the game's own. Light adds one, Full as many as fit. Experimental.", was="Frame generation",
    choices=["Off", "Light", "Full"], flags="g")
row("ps5.framegenMethod", "choice", "X", "Generation Method",
    "How generated frames are made: by tracking how each part moved, with AMD FSR3 optical flow, or with a trained network.",
    was="Generation method", choices=["PSFlyCast", "AMD FSR3", "MihawkGen"], flags="g")
row("ps5.framegenDebug", "toggle", "X", "Show the Movement",
    "Show generated frames' motion in colour, to check AMD FSR3 optical flow.", was="Show the movement", flags="g")
row("ta.AutoSkipFrame", "choice", "X", "Automatic Frame Skipping",
    "Disabled: No frame skipping. Normal: Skip a frame when the GPU and CPU are both running slow. Maximum: Skip a frame when "
    "the GPU is running slow.", "f", was="Automatic frame skipping", choices=["Disabled", "Normal", "Maximum"], flags="g")
row("ta.skip", "range", "X", "Frame Skipping", "Number of frames to skip between two actually rendered frames", "F",
    was="Frame skipping", flags="g", lo=0, hi=6, step=1)
row("rend.DelayFrameSwapping", "toggle", "X", "Delay Frame Swapping",
    "Useful to avoid flashing screen or glitchy videos. Not recommended on slow platforms", "F", was="Delay frame swapping", flags="g")

section("On-Screen Display")
row("rend.ShowFPS", "toggle", "M", "Show FPS Counter", "Show on-screen frame/sec counter", "F", was="Show frame rate", flags="g")
row("rend.FloatVMUs", "toggle", "M", "Show VMU In-game", "Show the VMU LCD screens while in-game", "F",
    was="Memory card screen", flags="g")
row("ps5.vmuScreenSize", "range", "X", "VMU Screen Size", "Size of the VMU screen shown while in-game.",
    was="Memory card screen size", flags="g", demo=100, unit="%", lo=50, hi=300, step=25)

section("Rendering")
row("pvr.rend", "choice", "M", "Transparent Sorting",
    "Per Triangle: Sort transparent polygons per triangle. Fast but may produce graphical glitches. Per Strip: Faster but may "
    "produce graphical glitches. Per Pixel: Slower but accurate.", "f", was="Transparency sorting",
    choices=["Per Triangle", "Per Strip", "Per Pixel"], flags="g", demo=2)
row("rend.PerPixelLayers", "choice", "X", "Maximum Layers",
    "Maximum number of transparent layers. May need to be increased for some complex scenes. Decreasing it may improve "
    "performance.", "F", was="Per-pixel layers", choices=["8", "16", "32", "64", "96", "128"], flags="g", demo=4)
row("ps5.softRenderer", "toggle", "X", "Software Renderer",
    "Render each pixel on the CPU as the Dreamcast's graphics chip did. Slower, and limited to 640x480.", was="Software renderer", flags="g")
row("rend.EmulateFramebuffer", "toggle", "X", "Full Framebuffer Emulation",
    "Fully accurate VRAM framebuffer emulation. Helps games that directly access the framebuffer for special effects. Very slow "
    "and incompatible with upscaling and wide screen.", "F", was="Full framebuffer emulation", flags="g")
row("rend.RenderToTextureBuffer", "toggle", "X", "Copy Rendered Textures to VRAM",
    "Copy rendered-to textures back to VRAM. Slower but accurate", "F", was="Copy rendered textures to VRAM", flags="g")
row("rend.FixUpscaleBleedingEdge", "toggle", "X", "Fix Upscale Bleeding Edge",
    "Helps with texture bleeding case when upscaling. Disabling it can help if pixels are warping when upscaling in 2D games.",
    "f", was="Fix edge bleeding in upscaled pictures", flags="g", demo=1)
row("rend.NativeDepthInterpolation", "toggle", "X", "Native Depth Interpolation",
    "Helps with texture corruption and depth issues on AMD GPUs. Can also help Intel GPUs in some cases.", "F",
    was="Native depth interpolation", flags="g", demo=1)

section("Game Patches")
row("ps5.patch.60fps", "toggle", "M", "60 FPS Patch",
    "Modify the game so that it runs at 60 frames per second. Disable it if the game runs too fast.", was="60 FPS patch", flags="G")
row("ps5.patch.widescreen", "toggle", "M", "Widescreen Patch",
    "Modify the game so that it draws a 16:9 picture itself, for games Widescreen Game Cheats does not support.",
    was="Widescreen patch", flags="G")
row("ps5.drawdist", "choice", "M", "Object Draw Distance",
    "Draw objects and enemies from further away. Supported games only. Experimental.", was="Object draw distance",
    choices=["Off", "1.5x", "2x", "3x", "5x"], flags="G")

# ================================================================================ Sound
area("Sound", "Volume, the sound chip and the menus' sounds")
section("Sound")
row("aica.Volume", "range", "E", "Volume Level", "Adjust the emulator's audio level", "F", was="Volume",
    flags="g", demo=100, unit="%", lo=0, hi=100, step=5)
row("aica.DSPEnabled", "toggle", "M", "Enable DSP",
    "Enable the Dreamcast Digital Sound Processor. Only recommended on fast platforms", "F", was="DSP emulation", flags="g", demo=1)
row("aica.BufferSize", "choice", "X", "Audio Buffer",
    "Set the maximum audio latency. A larger buffer stops crackling when a game runs slow.", was="Audio buffer",
    choices=["Small", "Medium", "Large"], flags="g", demo=1)
row("ps5.soundOutput", "choice", "M", "Sound Output", "Stereo or mono, as set in the Dreamcast's system settings.",
    was="Sound output", choices=["Stereo", "Mono"], flags="g")
row("ps5.ffSound", "toggle", "M", "Sound While Fast-Forwarding",
    "Play the game's sound during fast forward, quieter. When disabled, fast forward is silent.", was="Sound while fast-forwarding", demo=1)
section("VMU")
row("aica.VmuSound", "toggle", "M", "Enable VMU Sounds", "Play VMU beeps when enabled.", "F", was="VMU sounds", flags="g", demo=1)
row("ps5.vmuSoundPlace", "choice", "X", "VMU Sound Output",
    "Where VMU beeps play: the TV, the speaker of the controller whose VMU beeps, or both.", was="VMU sound plays in",
    choices=["TV", "Controller", "Both"], flags="g")
section("Menus")
row("ps5.menuSounds", "toggle", "E", "Menu Sounds", "Play a soft sound when moving, selecting and going back.", was="Menu sounds", demo=1)
row("ps5.startupSound", "toggle", "M", "Startup Sound", "Play sounds/startup.wav with the startup animation.",
    was="Start-up sound", demo=1)

# ================================================================================ Controls
area("Controls", "Controllers, sticks, light guns and other devices")
section("Controller")
row("ps5.vibration", "range", "E", "Vibration", "Rumble power", was="Vibration", flags="g", demo=100, unit="%", lo=0, hi=100, step=10)
row("ps5.deadzone", "range", "M", "Dead Zone", "Minimum deflection to register as input", was="Stick dead zone",
    flags="g", demo=10, unit="%", lo=0, hi=40, step=2)
row("ps5.stickDpad", "choice", "M", "Left Stick as D-Pad",
    "The left stick also works as the D-pad. Automatic enables it for arcade games without an analog stick.",
    was="Left stick as D-pad", choices=["Off", "Automatic", "On"], flags="g", demo=1)
row("ps5.turbo", "choice", "M", "Turbo Button", "Repeatedly press the chosen button while it is held.", was="Turbo button",
    choices=["Off", "A", "B", "X", "Y"], flags="g")
row("ps5.turboSpeed", "range", "X", "Turbo Speed", "How many times a second a turbo button is pressed.", was="Turbo speed",
    flags="g", demo=10, lo=5, hi=30, step=1)
row("ps5.controllerType", "choice", "M", "Controller Type",
    "Standard is the Dreamcast controller. Second Stick also reports the right stick, which only a few games read.",
    was="Controller type", choices=["Standard", "Second Stick"], flags="g")
row("ps5.portA", "choice", "M", "Port A", "The device in port A: a controller, a light gun, or the Twin Stick.", was="Port A",
    choices=["Controller", "Light Gun", "Twin Stick"], flags="G")
row("ps5.slot1", "choice", "M", "Expansion Slot 1", "The device in the controller's first slot, usually a VMU.",
    was="Controller slot 1", choices=["VMU", "Rumble Pack", "Microphone", "None"], flags="g")
row("ps5.slot2", "choice", "M", "Expansion Slot 2", "The device in the controller's second slot. Rumble needs the rumble pack.",
    was="Controller slot 2", choices=["Rumble Pack", "VMU", "Microphone", "None"], flags="g")
row("ps5.micVolume", "range", "X", "Microphone Volume",
    "Volume of the DualSense microphone when it acts as the Dreamcast microphone.", was="Microphone volume",
    flags="g", demo=50, unit="%", lo=0, hi=100, step=10)
row("ps5.peripherals", "choice", "X", "Peripherals",
    "Plug in what the game's disc lists: a light gun, a keyboard or a mouse.", was="Peripherals",
    choices=["As the Game Says", "Controllers Only"], flags="g")
section("Players")
row("ps5.controllers", "info", "M", "Controllers", "Another player joins by pressing the PS button on a second DualSense.",
    was="Controllers")
row("ps5.lightBar", "toggle", "M", "Light Bar Colour", "Light the controller in its player's colour.",
    was="Light bar in the player's colour", demo=1)
row("ps5.vmuB", "choice", "X", "Player 2 VMU", "The device in the second controller's first slot.", was="Player 2 memory card",
    choices=["VMU", "None"], flags="g")
row("ps5.vmuC", "choice", "X", "Player 3 VMU", "The device in the third controller's first slot.", was="Player 3 memory card",
    choices=["VMU", "None"], flags="g")
row("ps5.vmuD", "choice", "X", "Player 4 VMU", "The device in the fourth controller's first slot.", was="Player 4 memory card",
    choices=["VMU", "None"], flags="g")
section("Buttons and Profiles")
row("ps5.profiles", "action", "E", "Controller Profiles",
    "Choose what each button, trigger and stick does, in four built-in profiles and four of your own.", was="Controller profiles")
row("ps5.profile", "choice", "M", "Controller Profile",
    "Use the controller alone, with the right stick as a mouse, or as a mouse and keyboard.", was="Controller profile",
    choices=["Controller", "Right Stick Mouse", "Mouse and Keyboard"], flags="g")
row("ps5.restoreLayout", "action", "M", "Restore Default Layout",
    "Restore the standard layout for console games, and the original button order for arcade games.",
    was="Restore the default layout")
row("ps5.circleConfirms", "toggle", "M", "Circle Confirms", "Circle chooses and Cross goes back, in the menus only.",
    was="Circle confirms")
row("ps5.menuVibration", "toggle", "M", "Menu Vibration", "Vibrate briefly when a game starts and when something cannot be done.",
    was="Vibration in the menus", demo=1)
section("Right Stick and Mouse")
row("ps5.mouseSpeed", "range", "M", "Mouse Sensitivity", "Mouse speed at full right-stick deflection.", was="Mouse speed",
    flags="g", demo=5, lo=1, hi=10, step=1)
row("ps5.mouseMove", "choice", "X", "Mouse Movement", "Move the mouse with the right stick, by turning the controller, or both.",
    was="Mouse movement", choices=["Right Stick", "Motion", "Both"], flags="g")
row("ps5.rsDeadzone", "range", "X", "Right Stick Dead Zone", "Minimum right-stick deflection to register as input.",
    was="Right stick dead zone", flags="g", demo=10, unit="%", lo=0, hi=40, step=2)
row("ps5.rsResponse", "choice", "X", "Right Stick Response",
    "How deflection becomes movement. Precise is finer near the centre; Aggressive is quicker.", was="Right stick response",
    choices=["Linear", "Precise", "Aggressive"], flags="g")
row("ps5.rsSmoothing", "range", "X", "Right Stick Smoothing", "Blend in part of the previous frame's input to soften jitter.",
    was="Right stick smoothing", flags="g", demo=0, unit="%", lo=0, hi=90, step=10)
row("ps5.rsX", "choice", "X", "Right Stick Horizontal", "Normal, inverted or disabled.", was="Right stick left and right",
    choices=["Normal", "Inverted", "Disabled"], flags="g")
row("ps5.rsY", "choice", "X", "Right Stick Vertical", "Normal, inverted or disabled.", was="Right stick up and down",
    choices=["Normal", "Inverted", "Disabled"], flags="g")
row("ps5.rsTurn", "toggle", "X", "Right Stick Turns",
    "Add the right stick's horizontal movement to the left stick, so games without second-stick support can turn with it.",
    was="Right stick turns like the left stick", flags="g")
row("ps5.rsTriggers", "toggle", "X", "Right Stick as Triggers",
    "The right stick's horizontal movement presses the L and R triggers.", was="Right stick as the triggers", flags="g")
section("Light Gun")
row("ps5.gunAim", "choice", "M", "Light Gun Aiming", "Aim with the left stick, the touch pad, or by turning the controller.",
    was="Light gun aiming", choices=["Left Stick", "Touch Pad", "Motion"], flags="g")
row("ps5.motionDir", "choice", "X", "Motion Aiming Direction", "Swap directions if the gun moves the wrong way.",
    was="Motion aiming direction", choices=["Normal", "Swapped"], flags="g")
row("ps5.motionSpeed", "range", "X", "Motion Aiming Speed", "How far the gun moves when the controller turns.",
    was="Motion aiming speed", flags="g", demo=100, unit="%", lo=25, hi=300, step=25)
row("ps5.motionSmooth", "range", "X", "Motion Aiming Smoothing", "Steady the aim when the controller is held still.",
    was="Motion aiming smoothing", flags="g", demo=50, unit="%", lo=0, hi=100, step=10)
row("ps5.crosshair", "toggle", "M", "Crosshair", "Show where each gun points.", "f", was="Light gun crosshair", flags="g", demo=1)
section("Other Devices")
row("ps5.move", "toggle", "M", "PlayStation Move",
    "Use PS Move and Navigation controllers connected by the Move Management payload. Experimental.", was="PlayStation Move")
row("ps5.usbInput", "toggle", "M", "USB Keyboard and Mouse", "Use a USB keyboard and mouse plugged into the console.",
    was="USB keyboard and mouse", flags="n", demo=1)

# ================================================================================ Games and saves
area("Games and Saves", "The library, folders, save states and VMUs")
section("Library")
row("ps5.openOn", "choice", "M", "Open On", "Show Home or the Library when PSFlyCast starts.", was="Open on",
    choices=["Home", "Library"])
row("ps5.view", "choice", "E", "Library View", "Show the games as shelves, a grid or a list.", was="Library view",
    choices=["Shelves", "Grid", "List"], demo=1)
row("ps5.groupDiscs", "toggle", "M", "Group Discs", "Show a multi-disc game as one entry.", was="Group the discs of a game", demo=1)
row("ps5.showHidden", "toggle", "X", "Show Hidden Games", "List hidden games again, dimmed, so they can be restored.",
    was="Show hidden games")
row("ps5.hiddenInfo", "info", "X", "Hidden Games", "Hide a game from its page, under More. Hidden games appear in no list.",
    was="Hidden games")
row("ps5.scan", "action", "E", "Rescan Content", "Look for new games in every game folder.", "f", was="Scan for games")
row("ps5.covers", "toggle", "M", "Fetch Box Art",
    "Fetch covers from the libretro thumbnails collection, and descriptions from TheGamesDB.net.", "f",
    was="Download covers and descriptions", demo=1)
row("ps5.gameVideos", "choice", "M", "Game Videos", "Play a game's video in place of its cover, when one is found.",
    was="Game videos", choices=["Off", "Silent", "Quiet", "Medium", "Loud"], demo=1)
section("Folders")
row("ps5.lookForFiles", "action", "E", "Look For My Files",
    "Look through a drive, a share or a folder, and use the game, BIOS, save state, box art, texture and cheat folders found there.",
    was="Look For My Files")
row("ps5.addGameFolder", "action", "E", "Add Game Folder", "Add a folder of games on a USB drive, a network share or the console.",
    "f", was="Add Game Folder")
row("ps5.sharesInfo", "info", "X", "Folders on Shares", "Folders on network shares, added in Network.", was="Folders on shares")
row("folders.bios", "action", "M", "BIOS Folders", "Folders containing BIOS files (e.g. dc_boot.bin or dc_bios.bin) and arcade BIOS",
    "F", was="BIOS Folders")
row("folders.state", "action", "M", "Savestate Folders",
    "Folders for save states. First path is used for new states; all are searched when loading", "F", was="Savestate Folders")
row("folders.cover", "action", "M", "Box Art Folder", "Folder containing box art images (png/jpg).", "f", was="Box Art Folder")
row("folders.texture", "action", "X", "Texture Pack Folders",
    "Folders containing textures/<gameId> or <gameId> under a textures subfolder", "F", was="Texture Pack Folders")
row("folders.cheat", "action", "X", "Cheat Folders",
    "Folders containing cheat files (.cht/.txt) named with the game ID. Flycast will auto-load matching files if present", "F",
    was="Cheat Folders")
row("folders.video", "action", "X", "Game Video Folders", "Folders containing game videos (.mp4) named after the games.",
    was="Game Video Folders")
section("Where Files Are")
row("where.games", "info", "X", "Games Folder", "PSFlyCast's own games folder.", was="Games folder")
row("where.bios", "info", "X", "BIOS Folder", "The first BIOS folder in use.", was="BIOS folder")
row("where.covers", "info", "X", "Covers Folder", "Where box art is saved.", was="Covers folder")
row("where.cheats", "info", "X", "Cheats Folder", "The first cheat folder in use.", was="Cheats folder")
row("where.states", "info", "X", "Saves and States", "Where save states are written.", was="Saves and states")
row("where.vmu", "info", "X", "VMU Files", "VMU saves exported as files, and files to import.", was="Save files in and out")
section("Save States")
row("Dreamcast.AutoSaveState", "toggle", "E", "Auto Save State", "Save the state of the game when stopping", "F", was="Auto save state",
    flags="g", demo=1)
row("Dreamcast.AutoLoadState", "toggle", "M", "Auto Load State", "Load the last saved state of the game when starting", "F",
    was="Auto load state", flags="g")
row("ps5.rewind", "toggle", "M", "Rewind", "Keep the last three minutes of play to go back into. Experimental.", was="Rewind")
section("VMU")
row("Dreamcast.PerGameVmu", "toggle", "M", "Per Game VMU A1", "When enabled, each game has its own VMU on port 1 of controller A.", "F",
    was="Own memory card for each game", flags="g", demo=1)
row("ps5.vmuManager", "action", "E", "VMU Manager", "Copy, delete, import and export the saves on each VMU.", was="Memory cards")
row("ps5.cardSync", "toggle", "M", "Sync VMUs With the Share",
    "Keep the VMUs here and on the network share the same, at startup and when a game is closed.",
    was="Keep memory cards in sync with the network share", demo=1)
row("ps5.cardSyncNow", "action", "M", "Sync VMUs Now", "Make the VMUs here and on the share the same now.",
    was="Sync memory cards now")
section("Drives")
row("ps5.usb", "toggle", "M", "USB Drives", "Read games from USB drives. Requires elfldr.", was="USB drives", flags="r", demo=1)
row("ps5.usbFolders", "info", "X", "USB Game Folders", "Game folders found on USB drives.", was="USB game folders")
row("ps5.discDrive", "toggle", "M", "USB Disc Drive", "Play a Dreamcast disc in a USB CD or DVD drive. Requires elfldr.",
    was="USB disc drive", flags="r")
row("ps5.discSpeed", "choice", "X", "Disc Drive Speed", "Quiet reads at 8x, Normal at 24x, Maximum as fast as the drive can.",
    was="Disc drive speed", choices=["Quiet", "Normal", "Maximum"], demo=2)
section("Free Up Space")
row("ps5.freeSpace", "info", "M", "Free Space", "Free space on the console's storage.", was="Free space")
row("ps5.clearArchives", "action", "M", "Clear Unpacked Archives", "Delete disc images extracted from archives. They are extracted again when needed.",
    was="Unpacked archives")
row("ps5.clearShaders", "action", "X", "Clear Shader Cache", "Delete compiled shaders. Games compile them again as they run.",
    was="Shader cache")
row("ps5.clearLogs", "action", "X", "Clear Log Files", "Delete the log files.", was="Log files")

# ================================================================================ Emulation
area("Emulation", "The Dreamcast itself: region, BIOS, CPU and speed")
section("System")
row("Dreamcast.Region", "choice", "E", "Region", "BIOS region", "F", was="Region",
    choices=["Japan", "USA", "Europe", "Default"], flags="g", demo=3)
row("Dreamcast.Language", "choice", "M", "Dreamcast Language", "The language as configured in the Dreamcast BIOS", "F",
    was="Language", choices=["Japanese", "English", "German", "French", "Spanish", "Italian", "Default"], flags="g", demo=6)
row("Dreamcast.Broadcast", "choice", "X", "Broadcast", "TV broadcasting standard for non-VGA modes", "F", was="Broadcast",
    choices=["NTSC", "PAL", "PAL/M", "PAL/N", "Default"], flags="g", demo=4)
row("Dreamcast.Cable", "choice", "M", "Cable", "Video connection type. VGA gives the sharpest picture; some games require TV.",
    was="Video cable", choices=["VGA", "RGB Component", "TV Composite"], flags="g")
row("Dreamcast.HLE_BIOS", "toggle", "M", "HLE BIOS", "Force high-level BIOS emulation", "F", was="Built-in BIOS", flags="g")
row("Dreamcast.FastGDRomLoad", "toggle", "M", "Fast GD-ROM Loading",
    "Speeds up GD-ROM loading. Inaccurate: a few games need it disabled.", "f", was="Fast disc loading", flags="g", demo=1)
row("Dreamcast.RamMod32MB", "toggle", "X", "Dreamcast 32MB RAM Mod", "Enables 32MB RAM Mod for Dreamcast. May affect compatibility",
    "F", was="32 MB of memory", flags="g")
row("ForceFreePlay", "toggle", "M", "Naomi Free Play", "Configure Naomi games in Free Play mode.", "F", was="Arcade free play",
    flags="g", demo=1)
section("CPU")
row("Dynarec.Enabled", "toggle", "X", "Dynarec", "Use the dynamic recompiler. Recommended in most cases", "F", was="CPU recompiler",
    flags="g", demo=1)
row("Sh4Clock", "range", "X", "SH4 Clock",
    "Over/Underclock the main SH4 CPU. Default is 200 MHz. Other values may crash, freeze or trigger unexpected nuclear reactions.",
    "F", was="CPU clock", flags="g", demo=200, unit=" MHz", lo=100, hi=300, step=10)
section("Speed")
row("ps5.ffSpeed", "choice", "M", "Fast Forward Speed", "How fast fast forward runs the game.", was="Fast forward speed",
    choices=["2x", "3x", "4x", "Unlimited"], demo=1)

# ================================================================================ Network
area("Network", "Shares, netplay, online games and achievements")
section("Network Shares")
row("ps5.ramCache", "toggle", "M", "Load Games Into Memory",
    "Read the whole game into memory before it starts, so the share can sleep. When disabled, the game is streamed.",
    was="Load network games into memory", demo=1)
row("ps5.netBuffers", "toggle", "X", "Large Network Buffers", "Use 4 MB network buffers for shares. Disable it if a share misbehaves.",
    was="Large network buffers", demo=1)
row("ps5.netFolders", "info", "M", "Network Folders", "SMB shares, as used by Windows file sharing and most NAS devices.",
    was="Network folders")
row("ps5.findServers", "action", "E", "Find Servers", "Look for computers and NAS devices that share folders on this network.",
    was="Find servers on the network")
row("ps5.addNetFolder", "action", "M", "Add Network Folder", "Enter server/share/folder, for example 192.168.1.10/Games/Dreamcast.",
    was="Add a network folder")
row("ps5.netUser", "action", "M", "Username", "The account for the shares. Use guest for an open share.", "f", was="Network user")
row("ps5.netPassword", "action", "M", "Password", "The password for the shares, saved in network.cfg.", "f", was="Network password")
row("ps5.netScan", "action", "M", "Rescan Network Folders", "Look for games on the shares again.", was="Scan the network folders now")
section("Netplay")
row("network.GGPO", "choice", "M", "Enable Netplay", "Enable networking using GGPO", "F", was="Netplay",
    choices=["Off", "Player 1", "Player 2"])
row("network.server", "action", "M", "Peer", "Your peer IP address. Each player enters the other's.", "f", was="Other player")
row("network.GGPODelay", "range", "M", "Frame Delay", "Sets Frame Delay, advisable for sessions with ping >100 ms", "F",
    was="Input delay", demo=0, lo=0, hi=20, step=1)
row("ps5.netStick", "choice", "X", "Analog in Netplay", "How much of the left stick is sent to the other player.",
    was="Left stick in netplay", choices=["Full", "D-Pad Only"])
row("ps5.thisConsole", "info", "M", "This Console", "The address the other player enters.", was="This console")
row("ps5.bothNeed", "info", "X", "Both Players Need", "The same game and the same save state, as <game>.state.net in data/savestates.",
    was="Both players need")
row("network.EnableUPnP", "toggle", "M", "Enable UPnP", "Automatically configure your network router for netplay", "F",
    was="Open the router's port (UPnP)", demo=1)
section("Dreamcast Online")
row("ps5.dcOnline", "choice", "M", "Online Connection",
    "How a game's own online mode connects: the modem or the Broadband Adapter, through DCNet or directly.",
    was="Dreamcast online", choices=["Modem", "Broadband Adapter", "Modem with DCNet", "Broadband Adapter with DCNet"])
row("network.ISPUsername", "action", "M", "ISP User Name", "The name a game's online mode signs in with.", "f", was="Name online")
section("Achievements")
row("achievements.Enabled", "toggle", "E", "Enable RetroAchievements", "Track your game achievements using RetroAchievements.org",
    "F", was="RetroAchievements", demo=1)
row("achievements.account", "action", "M", "Account", "Sign in with your RetroAchievements account.", was="Account")
row("achievements.mode", "info", "X", "Mode", "Save states, cheats, rewind and fast forward stay available. Hardcore mode is not offered.",
    was="Mode")
row("achievements.inGame", "info", "X", "In a Game", "Shows achievement popups ingame.", "f", was="In a game")

# ================================================================================ Interface
area("Interface", "How PSFlyCast itself looks and moves")
section("Look")
row("ps5.skin", "choice", "E", "Skin", "The colours of every screen.", was="Skin", choices=["Midnight", "Ember", "Pocket LCD", "Arcade", "Carbon"])
row("ps5.accent", "choice", "M", "Accent Colour", "The colour of focused items.", was="Accent colour",
    choices=["Skin's Own", "Blue", "Orange", "Green", "Teal", "Violet", "Magenta", "Red"])
row("ps5.backdrop", "choice", "M", "Background", "Show a still picture or an animation behind the menus.", was="Background",
    choices=["Skin's Own", "Still", "Aurora", "Waves", "Sparks", "Horizon", "Dot Matrix", "Cover Colours"])
row("ps5.motion", "choice", "M", "Motion", "All animates the menus; Less only fades; None does not animate.", was="Motion",
    choices=["All", "Less", "None"])
section("Accessibility")
row("ps5.uiScale", "choice", "E", "UI Scaling", "Adjust the size of UI elements and fonts.", "F", was="Interface size",
    choices=["70%", "80%", "90%", "100%", "110%", "120%", "130%"], demo=3)
row("ps5.highContrast", "toggle", "M", "High Contrast", "Use a black background and brighter text.", was="High contrast")
row("ps5.colourBlind", "toggle", "M", "Colour-Blind Colours", "Use blue and orange instead of green and red.",
    was="Colour-blind safe colours")
section("Startup")
row("ps5.splash", "choice", "M", "Startup Animation", "The animation shown when PSFlyCast starts. Random picks a different one each time.",
    was="Start-up animation", choices=["None", "Random", "Birds", "Dock", "Comet", "Shutter"], demo=1)
section("Screen Care")
row("ps5.oled", "choice", "M", "OLED Care",
    "Shift the picture a few pixels in a slow loop so nothing stays lit in one place. Dim also darkens the menus when idle.",
    was="OLED care", choices=["Off", "Pixel Shift", "Pixel Shift and Dim"])

# ================================================================================ System
area("System", "The console, logs, updates and what this build is")
section("Performance")
row("ps5.threadedRecording", "toggle", "X", "Threaded Graphics Driver", "Record graphics commands on a separate thread. Experimental.",
    was="Graphics driver on its own thread", flags="r")
row("ps5.gpuSpin", "range", "X", "GPU Wait: Spin", "How long a wait for the GPU keeps checking before it sleeps, in µs.",
    was="GPU wait: spin", flags="r", demo=1500, lo=0, hi=5000, step=250)
row("ps5.gpuSleep", "range", "X", "GPU Wait: Sleep", "How long that wait sleeps between checks, in µs.", was="GPU wait: sleep",
    flags="r", demo=1000, lo=100, hi=5000, step=100)
row("ps5.shortQueue", "toggle", "X", "Short Display Queue", "Keep at most two frames waiting for the screen. Experimental.",
    was="Short display queue")
row("ps5.priorities", "toggle", "X", "Sound and Controls First", "Run sound and the menus ahead of the emulator. Experimental.",
    was="Sound and controls first", flags="r")
row("ps5.cores", "toggle", "M", "Dedicated Cores", "Run the emulator, the renderer and the graphics driver each on a core of its own.",
    was="Threads on cores of their own", flags="r", demo=1)
row("ps5.driverCore", "toggle", "X", "Dedicated Driver Core", "Give the graphics driver's thread a core to itself.",
    was="Graphics driver on a core of its own", flags="r", demo=1)
section("Logs")
row("ps5.logs", "toggle", "M", "Write Logs", "Write the boot log and Flycast's log. When disabled, a crash leaves no report.",
    was="Write logs", flags="r", demo=1)
row("ps5.logPlace", "choice", "X", "Log Location", "Keep the logs in PSFlyCast's folder, or on a USB drive.", was="Keep logs",
    choices=["PSFlyCast's Folder", "USB Drive"], flags="r")
row("ps5.logSync", "choice", "X", "Log Saving", "How often the boot log is written to the drive.", was="Save logs to the drive",
    choices=["Every 2 s", "Every 15 s", "On Exit", "Kept in Memory"], flags="r", demo=1)
row("ps5.logNow", "action", "X", "Write Log Now", "Write the boot log kept in memory to the drive now.", was="Write the log now")
row("ps5.bootLog", "info", "X", "Boot Log", "Where this run's boot log is.", was="Boot log")
section("Updates")
row("ps5.checkUpdate", "action", "E", "Check for Updates", "Check for a newer release, which can then be downloaded and installed.",
    was="Check for updates")
row("ps5.updateAtStart", "toggle", "M", "Check at Startup", "Check for a newer release once at startup.", was="Look for updates at start-up", demo=1)
section("About")
row("about.version", "info", "E", "Version", "This build of PSFlyCast.", was="PSFlyCast")
row("about.graphics", "info", "X", "Graphics", "The graphics driver in use.", was="Graphics")
row("about.display", "info", "X", "Display", "The display mode in use.", was="Display")
row("about.bios", "info", "X", "Dreamcast BIOS", "The BIOS in use.", was="Dreamcast BIOS")
row("about.storage", "info", "X", "Storage", "Where PSFlyCast keeps its files.", was="Storage")
row("about.credits", "info", "X", "Credits", "Flycast and every part PSFlyCast is made of.", was="Credits")
row("about.quit", "action", "M", "Exit", "Close PSFlyCast.", "f", was="Quit PSFlyCast")


# -------------------------------------------------------------------------------- output
def rows():
    for a, _, secs in AREAS:
        for s, rs in secs:
            for r in rs:
                yield a, s, r


def cstr(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def bake():
    out = ["// Generated by shell/ps5/tools/settings-table.py bake: do not edit; change the table there.",
           "// SPDX-License-Identifier: GPL-2.0-or-later", ""]
    out.append("const Area areas_[] = {")
    for a, blurb, secs in AREAS:
        out.append(f"\t{{ {cstr(a)}, {cstr(blurb)} }},")
    out.append("};")
    out.append("const Setting table[] = {")
    for a, s, r in rows():
        ai = [x[0] for x in AREAS].index(a)
        choices = "{ " + ", ".join(cstr(c) for c in r["choices"]) + " }" if r["choices"] else "{}"
        out.append(f"\t{{ {cstr(r['id'])}, {ai}, {cstr(s)}, Kind::{r['kind'].capitalize()}, Level::{ {'E':'Essentials','M':'More','X':'Everything'}[r['level']] }, "
                   f"{cstr(r['label'])}, {cstr(r['desc'])}, '{r['src']}', {cstr(r['flags'])}, {choices}, "
                   f"{r['demo']}, {r['lo']}, {r['hi']}, {r['step']}, {cstr(r['unit'])} }},")
    out.append("};")
    return "\n".join(out) + "\n"


def review():
    n = sum(1 for _ in rows())
    src = {"F": 0, "f": 0, "P": 0}
    for _, _, r in rows():
        src[r["src"]] += 1
    out = ["# Every setting's words", "",
           f"Generated by `shell/ps5/tools/settings-table.py review` from the table in that script: {n} settings in "
           f"{len(AREAS)} areas. Source **F**: Flycast's label and description word for word ({src['F']}); **f**: Flycast's "
           f"own words joined or shortened ({src['f']}); **P**: PSFlyCast's, written as Flycast writes them ({src['P']}). "
           "Level: E Essentials, M More, X Everything. Flags: g a game can have its own value, G only a game has it, "
           "r takes effect at the next start of PSFlyCast, n at the next game. *Was* is the label before.", ""]
    for a, blurb, secs in AREAS:
        out.append(f"## {a}")
        out.append("")
        out.append(blurb + ".")
        out.append("")
        for s, rs in secs:
            out.append(f"### {s}")
            out.append("")
            out.append("| Label | Description | Src | Lvl | Flags | Choices | Was |")
            out.append("| --- | --- | --- | --- | --- | --- | --- |")
            for r in rs:
                ch = ", ".join(r["choices"]) if r["choices"] else (f"{r['lo']}–{r['hi']}{r['unit']}" if r["kind"] == "range" else "")
                was = "" if r["was"] == r["label"] else r["was"]
                out.append(f"| **{r['label']}** | {r['desc'].replace('|', '/')} | {r['src']} | {r['level']} | {r['flags']} | {ch} | {was} |")
            out.append("")
    return "\n".join(out)


def check():
    problems = []
    ids = [r["id"] for _, _, r in rows()]
    dup = {i for i in ids if ids.count(i) > 1}
    if dup:
        problems.append("ids twice: " + ", ".join(sorted(dup)))
    if OLD.exists():
        was = {r["was"] for _, _, r in rows()} | {r["label"] for _, _, r in rows()}
        for line in OLD.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#") and line not in was:
                problems.append("no place for the old setting: " + line)
    if INC.exists() and INC.read_text(encoding="utf-8") != bake():
        problems.append(f"{INC.relative_to(ROOT)} is not what bake writes: run settings-table.py bake")
    for p in problems:
        print(p)
    n = len(ids)
    print(f"settings table: {n} settings, {len(AREAS)} areas, {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "bake":
        INC.parent.mkdir(parents=True, exist_ok=True)
        INC.write_text(bake(), encoding="utf-8")
        print("wrote", INC.relative_to(ROOT))
    elif cmd == "review":
        REVIEW.write_text(review(), encoding="utf-8")
        print("wrote", REVIEW.relative_to(ROOT))
    elif cmd == "json":
        print(json.dumps([dict(area=a, section=s, **r) for a, s, r in rows()], indent=1))
    else:
        sys.exit(check())
