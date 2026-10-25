### repo: pushes straight to main are checked for a changelog note too

The `changelog` workflow now also runs on every push to `main`, judged against the commit `main` pointed at before the push, so a direct commit needs the same `changelog.d/` note a pull request does. Releases use a pull request carrying `skip-changelog`; a direct release push is refused because push events cannot carry that exception.
