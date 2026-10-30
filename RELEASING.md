# Releasing

Releases are automated. There is no `gh` login, no PAT, and no interactive step:
push a tag and GitHub builds the firmware and publishes the release.

## Cut a release

1. Bump the version in `src/modules/sysinfo.cpp` (`FW_VERSION`) and the three
   flashing references in `README.md` (`T-Deck-Ai-Terminal-vX.Y.Z.bin`).
2. Commit and push `main`.
3. Create an annotated tag whose message is the release title, a blank line, then
   the notes — the first line becomes the GitHub release title, the rest the body:

   ```txt
   Single-Endpoint Chat

   AI chat now takes one URL.

   - ...
   ```

   ```sh
   git tag -a vX.Y.Z -F tagmsg.txt
   git push origin vX.Y.Z
   ```

4. Watch the **Release** workflow under the repo's Actions tab. It builds with
   PlatformIO and attaches `T-Deck-Ai-Terminal-vX.Y.Z.bin` to the release.

Keep the title to three words. Nothing else is required.

## What the workflow does

`.github/workflows/release.yml`, on any `v*` tag:

- builds the firmware with `pio run`;
- re-fetches tags (`git fetch --force --tags origin`) because `actions/checkout`
  can materialise the tag as a lightweight ref and hide the annotated message;
- reads the tag's first line as the title and the remainder as the notes;
- creates the release with the repo's automatic `GITHUB_TOKEN`, or, if the release
  already exists, replaces the asset and edits the title/notes.

The binary is gitignored on purpose: it lives in the release, not in the tree.

## Re-releasing the same version

**Do not move a tag that already has a release.** GitHub deletes the release when
its tag is moved or deleted. The usual run takes about two minutes.

If a release is missing (for example after a tag was moved), push any commit to
`main`. The workflow notices the newest tag has no release and rebuilds it. That
repair path is why `main` pushes also trigger the workflow; when the release
already exists they exit in seconds without building.

If you must rebuild and replace the asset for the same tag:

```sh
git tag -f -a vX.Y.Z -F tagmsg.txt
git push --force origin vX.Y.Z     # this deletes the existing release
# then repair it without touching the tag again:
git commit --allow-empty -m "Repair vX.Y.Z release"
git push origin main
```

## Local build (optional)

```sh
pio run
copy .pio\build\T-Deck\firmware.bin F:\T-Deck-Ai-Terminal-vX.Y.Z.bin
```

This is for testing only. The release binary is built by CI.
