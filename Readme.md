# TribesViewer

## What is it?

TribesViewer is a simple viewer for Tribes 1 and Starsiege ".dts" models.

## Sounds great, how do i use it?

Assuming you have compiled the executable, you need to supply a list of paths or volumes in which the "dts" and associated asset files are located, in addition to a palette ".ppl" file to use. The final parameter should then be the model file you want to view to start off with. e.g.


	./TribesViewer . Entities.vol ice.day.ppl harmor.dts


A simple GUI is also provided so you can change sequences, detail levels, and load different models straight from the supplied volumes and directory paths.

Note that as of yet, there are still a few bugs present so don't expect everything to render flawlessly. Player models should function correctly.

Model and volume files from earlier Dynamix games are currently not supported.
