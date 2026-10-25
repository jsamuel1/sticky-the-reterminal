# changelog.d — one note per change

Pull requests and pushes to `main` do not edit `CHANGELOG.md`. Each change adds
a small file here, and
the notes are gathered into `CHANGELOG.md` when a firmware version is released.
Two open pull requests never touch the same file, so they never conflict on the
changelog. The tool is [changelog.d](https://cagataycali.github.io/changelog.d/).

## Adding a note

Open the pull request first so you know its number, then:

```sh
pip install git+https://github.com/cagataycali/changelog.d
changelog-d new 42 firmware "the lock chord works with the case closed"
# → changelog.d/42-the-lock-chord-works-with-the-case-closed.md
changelog-d check
```

Or write the file by hand — `changelog.d/<pr-number>-<short-name>.md`,
lowercase words with dashes:

```markdown
### firmware: the lock chord works with the case closed

UP+DOWN was read through the case's flex point and missed one press in five.
```

The rules:

- The first line is a `### <area>: <what changed>` heading. It becomes the
  entry. Use the area words this changelog already uses: `firmware`,
  `grammar vN`, `docs`, `repo`, `rollout`, `tools`.
- Say what a user or an agent driving the device would notice. Anything that
  changes a verb, its args or a reply shape also bumps `TINY_GRAMMAR_VERSION`
  and says so in the note (`### grammar v13: …`).
- One change per file. No `##` headings — those are versions.
- Use a real number: the PR's. Open a pull request before naming its fragment;
  direct pushes to `main` are checked but are not the normal landing path.
  `0000-wip.md`-style names are refused.
- Only notes and this README live here; any other file is an error.

Nothing a user would notice (typo, CI, comment)? Ask for the
`skip-changelog` label instead. Changes, including releases, land through pull
requests so the exception stays explicit and reviewable.

## Checking

`changelog-d check` validates every note here; `changelog-d gate --base-ref
origin/main` runs the same check the `changelog` workflow
(`.github/workflows/changelog.yml`) runs on every pull request and every push
to `main`.

## Releasing

`tools/changelog.sh release "<version> — grammar vN — <name>"` folds every
note into a new `CHANGELOG.md` section and deletes them. Put that release commit
in a pull request carrying `skip-changelog`; direct pushes cannot carry this
explicit exception and are refused. See
[docs/release.md](../docs/release.md#changelog-at-release).
