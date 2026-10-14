# Releasing pyaio

## One-time setup (maintainer)

1. **Private vulnerability reporting:** in the GitHub repository, go to
   *Settings, Code security*, then *Private vulnerability reporting*, and
   choose *Enable*. `SECURITY.md` points reporters there. With the GitHub CLI:
   `gh api -X PUT repos/felipecruz/pyaio/private-vulnerability-reporting`
2. **PyPI trusted publishing:** on PyPI, add a trusted publisher for project
   `pyaio` with owner `felipecruz`, repository `pyaio`, workflow
   `release.yml` and environment `pypi`.
3. **GitHub environment:** create an environment named `pypi` (*Settings,
   Environments*). Optionally require a manual approval for it.

## Every release

1. Make sure CI is green on `main`. That covers Python 3.10 to 3.14 on
   Ubuntu 22.04 and 24.04, the sanitizer job, the seccomp fallback job and
   the package check.
2. Set the version in `pyaio/__init__.py` (PEP 440, for example `0.5.0rc1`,
   `0.5.0`).
3. Move the `CHANGELOG.md` entries under that version and date them.
4. Commit, then tag and push:
   ```sh
   git tag -a v0.5.0rc1 -m "pyaio 0.5.0rc1"
   git push origin main v0.5.0rc1
   ```
5. The `Release` workflow builds manylinux wheels (x86_64 and aarch64, with
   liburing bundled), tests them, builds the sdist and publishes everything.
   pip only installs a release-candidate version such as `0.5.0rc1` when
   asked: `pip install --pre pyaio`.
6. Check the PyPI page, then install from it in a clean virtual environment
   and run the tests from `tests/`.

To try the build without publishing, run the `Release` workflow by hand
(*Actions, Release, Run workflow*). Publishing only happens for `v*` tags.

## Not covered by CI

- **Older kernels.** GitHub-hosted runners use recent kernels. pyaio's minimum
  is 5.6, and cancellation was written to work without 5.19's
  `IORING_ASYNC_CANCEL_ANY`, but neither has been run on an old kernel. Booting
  an LTS kernel (5.15, 6.1) with virtme-ng/QEMU on a KVM-capable runner would
  close this gap.
- **musllinux wheels** are not built yet.
- **Abandoning a request on close timeout** has no test, because there is
  no reliable way to create a request the kernel refuses to cancel.
