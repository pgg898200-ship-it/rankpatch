# RankPatch 0.1.0 binary wheels

The wheel workflow builds the published v0.1.0 source for standard CPython 3.11, 3.12 and 3.13. Its target platforms are Linux x86-64 with glibc 2.28 or newer, Apple Silicon with macOS 11 or newer, and Windows x86-64.

After the wheel workflow succeeds, install the matching release asset with

```sh
python -m pip install --no-index --only-binary=:all: --find-links https://github.com/pgg898200-ship-it/rankpatch/releases/expanded_assets/v0.1.0 rankpatch==0.1.0
```

The workflow tests both ranking strategies against each newly built wheel. Its final jobs repeat the public install command on all nine supported interpreter and platform combinations. Maintainers can rerun the workflow to complete a partial upload. Completed release assets are reused and their contents checked before any missing wheels are attached.

[Wheel build status](https://github.com/pgg898200-ship-it/rankpatch/actions/workflows/wheels.yml) · [Release assets](https://github.com/pgg898200-ship-it/rankpatch/releases/tag/v0.1.0)
