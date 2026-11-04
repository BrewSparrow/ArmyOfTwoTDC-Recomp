# assets/

Put the extracted contents of your own *Army of Two: The Devil's Cartel* Xbox 360 disc here.

Dump the disc with ABGX360 (or unpack an existing ISO with extract-xiso), then copy everything
from the extracted disc root straight into this folder, so that `default.xex` sits at
`assets/default.xex`:

```
assets/
  $SystemUpdate/
  Content/
  Data/
  Movies/
  AO4.Main.xex
  AO4.Splash.xex
  default.xex          (Default.xex on some extractions; either works on Windows)
  Engine.BuildInfo.xex
  NxeArt
  README.md            (this file)
```

The build recompiles `default.xex`, `AO4.Main.xex` and `Engine.BuildInfo.xex` from here, and the
game reads its data from here at run time. Nothing in this folder except this file is tracked by
git - never commit game files. See the main README for the values to verify your dump against.
