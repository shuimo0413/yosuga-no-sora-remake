# rawfile

The game content lives in the repository root `data/` directory and is exposed
to the build as the `data` entry under this folder:

- On Windows a directory junction `data` pointing at the repository
  `data/` directory is created by `tools/setup_ohos_project.py`.
- On Linux/macOS the same script creates a symbolic link.

The native entry code extracts the packaged rawfile content into the
application sandbox on first launch, where the Kirikiri engine reads it as a
normal filesystem tree. See `ohos-project/README.md` for details.
