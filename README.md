# Gorilla Quake

Gorilla Quake is an unofficial fork of Vittorio Romeo's [Quake VR](https://vittorioromeo.com/quakevr) that adds
Gorilla Locomotion: a VR hand-based movement mode inspired by Gorilla Tag. When
enabled, the player can plant their hands against level geometry and push their
body away from the surface, including jumping, climbing, and hand-driven
movement while swimming.

The original Quake VR features and installation model still apply. This fork
does not include the commercial Quake data files; players must provide their own
`PAK0.PAK` and `PAK1.PAK` from a legitimate Quake installation.

## Demo

[![GorillaQuake gameplay demo](https://img.youtube.com/vi/etGrZx4Dxc0/maxresdefault.jpg)](https://www.youtube.com/watch?v=etGrZx4Dxc0)

## Installation

1. Install ["Visual C++ x64 Redistributable"](https://support.microsoft.com/en-gb/help/2977003/the-latest-supported-visual-c-downloads) package.

2. Install [7zip](https://www.7-zip.org/)

3. Install "Quake". The [Steam version](https://store.steampowered.com/app/2310/QUAKE/) works well.

4. Grab the latest binary from [the releases page](https://github.com/SuperV1234/quakevr/releases).

5. Extract the Quake VR archive into a new, empty folder *(e.g. `C:/Games/id/Quake/quakevr`)*.

6. Navigate to the `id1` folder in your installation of Quake *(e.g. `C:/Program Files/Steam/steamapps/common/Quake/id1`)* and copy both `PAK0.PAK` and `PAK1.PAK` to Quake VR's `id1` folder *(e.g. `C:/Games/id/Quake/quakevr/id1`)*.

7. Run the `quakevr.exe` executable in the root folder to launch the game!

8. (optional) To enable music, copy the `music` folder from the 'remaster' *(e.g. `C:/Program Files/Steam/steamapps/common/Quake/rerelease/id1/music`)* to Quake VR's `id1` folder as well.

## First Steps

### SteamVR Bindings

The first thing you should do after starting Quake VR is opening the *"Controller Bindings"* interface on SteamVR and ensure that in-game actions are mapped to the motion controllers. There are two action sets to bind: one for in-game actions, and one for menu control. See an [example video **here**](https://giant.gfycat.com/ThornyEducatedBushbaby.mp4).

### In-Game Configuration

After setting up your bindings, please go through all the options in *"Quake VR Settings"*, and tweak the game to your liking. Do not forget to:

* Calibrate your height;
* Tweak the position of the *"VR torso"* and of the holsters (*"hotspots"*);
* Go through the *"Immersion Settings*".

There is no "best" way of playing Quake VR. Simply use the settings that you enjoy the most!

## Multiplayer

### Versus Bots

To play against bots, select *"Multiplayer & Bots"* from the main menu, then *"New Game"* and follow the on-screen instructions. When in-game, spawn new bots from the *"Bot Control"* menu under *"Multiplayer & Bots"*.