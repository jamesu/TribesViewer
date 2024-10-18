# TribesViewer

## What is it?

TribesViewer is a simple viewer for Tribes 1 and Starsiege ".dts" models, with additional support for loading `.dis` interior files, and '.ter' terrain files.

## Requirements

Currently the following is needed to build:

- SDL3
- wgpu-native

## Building

	mkdir build
	cd build
	cmake -DUSE_WGPU_NATIVE=1 -DWGPU_NATIVE_PATH=path/to/wgpu-native ..
	make

## Usage

Assuming you have compiled the executable, you need to supply a list of paths or volumes in which the "dts"/"dis" and associated asset files are located, in addition to a palette ".ppl" file to use. The final parameter should then be the model file you want to view to start off with. e.g.


	./TribesViewer . Entities.vol ice.day.ppl harmor.dts


A simple GUI is also provided so you can change sequences, detail levels, and load different models straight from the supplied volumes and directory paths.

For terrains there is currently no mechanism to auto-detect volumes required for each mission, so if you want to load terrains properly you will need to specify the DML, Terrain, and World volumes for the terrain you wish to view along with the correct palette.  e.g.


	./TribesViewer . alienDML.vol alienTerrain.vol AntHill.ted alienWorld.vol alien.day.ppl AntHill.dtf


Note that as of yet, there are still a few bugs present so don't expect everything to render flawlessly. Player models should function correctly.

Model and volume files from earlier Dynamix games are currently not supported.
