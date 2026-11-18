# dlc/

Optional. Put the DLC packages for Army of Two: The Devil's Cartel here, exactly as copied from
your console's storage (`Content\0000000000000000\454109AB\00000002\`), for example:

- `8BBB0270061C248F30443C9885CB502300E2C03D45` (Hitmakers Pack)
- `9DDFDADC8879D83BEE746E3C54945866E095576745` (Online Pass and Overkill Edition content)

When the game starts, each package is installed into `user\` the way a console installs it to
its hard drive; the game then finds the content itself. No rebuild is needed after adding or
removing packages. Avatar items (`00009000`) are not supported. Online features do not work.

The HD texture pack is not needed here: it is part of the disc dump and is installed from
`assets\Content\...\454109AB00000000` automatically (a copy in this folder is skipped).

Nothing in this folder is tracked by git or included in a release.
