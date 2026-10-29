Release Process
====================

Releases are normally prepared on `master`, without release candidates or a
separate release branch.

## Versioning and Bitcoin Core compatibility

Versions use `MAJOR.MINOR.PATCH`, with `CLIENT_VERSION_BUILD` holding the patch
number in [CMakeLists.txt](../CMakeLists.txt).

* When dropping support for an older Bitcoin Core major version, increment
  `CLIENT_VERSION_MINOR` and reset `CLIENT_VERSION_BUILD` to `0`. For example,
  `v1.1.0` required Core v31, while `v1.0.6` supported Core v30.2.
* For other releases, increment the patch version. Adding support for a newer
  Core version while retaining existing compatibility can be a patch release:
  `v1.1.2` supports both Core v31 and v32.
* State supported Core versions, including minimum patch versions and any
  required upgrade order, in the README and release notes. Do not assume that
  untested later Core versions will remain compatible.
* Keep `CLIENT_VERSION_RC` at `0`.

## Before every release

* Check every subtree for new upstream release tags before preparing the release.
  See the [subtree inventory](../test/lint/README.md#git-subtree-checksh) for the
  subtree paths and upstream repositories (currently `src/ipc/libmultiprocess`
  and `src/secp256k1`).
* Query each upstream repository's tags, for example with
  `git ls-remote --tags <upstream-url>`, and compare its latest release tag with
  the upstream revision recorded in the latest subtree update's
  `git-subtree-split` metadata. If the tagged release is already included, no
  update is needed. New commits on an upstream branch alone do not require an
  update; only new release tags qualify.
* If a newer release tag is available, update the subtree to that tag in a
  separate pull request, following the [subtree update
  instructions](developer-notes.md#subtrees). Use the release tag as the ref in
  `git subtree pull`, rather than the upstream branch tip, and run
  `test/lint/git-subtree-check.sh -r <prefix>` after fetching the upstream ref.
* Merge the subtree update pull requests into `master` before proceeding with
  release preparation. Keep subtree updates separate from release preparation
  changes. If an upstream has no release tags, leave that subtree unchanged.
* Check CI results for the intended release commit, including SRI integration
  tests against the supported Core versions. Keep the compatibility claims and
  [integration test matrix](../.github/workflows/sri-integration-test.yml) aligned.

## Prepare and tag

1. Start from a clean `master` and choose the release version using the rules
   above. The patch number is normally already set by the previous release's
   development-version bump.
2. Update [doc/release-notes.md](release-notes.md) for this release, using the
   changes since the previous release tag. Summarize notable changes, link the
   relevant PRs, credit contributors, and explain Core compatibility and upgrade
   requirements. Update the README if compatibility or usage has changed.
   Previous notes remain available through release tags and GitHub releases;
   no wiki or separate archive copies are needed.
3. Set the chosen version in `CMakeLists.txt` and set
   `CLIENT_VERSION_IS_RELEASE` to `true`. Configure and rebuild locally:

   ```sh
   cmake --preset dev-mode -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j16
   ```

4. Regenerate the manpage and example configuration, then review the output:

   ```sh
   contrib/devtools/gen-manpages.py
   contrib/devtools/gen-conf.sh
   build/bin/sv2-tp --version
   test/lint/check-doc.py
   test/lint/lint-includes.py
   git diff --check
   ```

   Confirm the binary and manpage show the intended release version. Commit
   the version, documentation, and generated changes together with subject
   `build: prepare vX.Y.Z release`. Verify CI for the final preparation commit
   before tagging it.
5. Create a signed annotated tag on the preparation commit. For example, with
   `VERSION=1.1.3` and that commit checked out:

   ```sh
   git tag -s "v${VERSION}" -m "v${VERSION}"
   ```

   Publish the release tag for builders to fetch. Build from this tag throughout
   the remaining steps.
6. On `master`, increment `CLIENT_VERSION_BUILD` and set
   `CLIENT_VERSION_IS_RELEASE` back to `false`. Commit with subject
   `build: unset release flag and bump patch version`. This development commit
   is not part of the tagged release.

## Building

### First time / New builders

Install Guix using one of the installation methods detailed in
[contrib/guix/INSTALL.md](/contrib/guix/INSTALL.md).

Check out the source code in the following directory hierarchy.

    cd /path/to/your/toplevel/build
    git clone https://github.com/stratum-mining/sv2-tp-guix.sigs.git
    git clone https://github.com/stratum-mining/sv2-tp-detached-sigs.git
    git clone https://github.com/stratum-mining/sv2-tp.git

### Setup and perform Guix builds

Checkout the Template Provider version you'd like to build:

```sh
pushd ./bitcoin
SIGNER='(your builder key, ie bluematt, sipa, etc)'
VERSION='(new version without v-prefix, e.g. 25.0)'
git fetch origin "v${VERSION}"
git checkout "v${VERSION}"
popd
```

Ensure your guix.sigs are up-to-date if you wish to `guix-verify` your builds
against other `guix-attest` signatures.

```sh
git -C ./guix.sigs pull
```

### Create the macOS SDK tarball (first time, or when SDK version changes)

Create the macOS SDK tarball, see the [macdeploy
instructions](/contrib/macdeploy/README.md#sdk-extraction) for
details.

### Build and attest to build outputs

Follow the relevant Guix README.md sections:
- [Building](/contrib/guix/README.md#building)
- [Attesting to build outputs](/contrib/guix/README.md#attesting-to-build-outputs)

### Verify other builders' signatures to your own (optional)

- [Verifying build output attestations](/contrib/guix/README.md#verifying-build-output-attestations)

### Commit your non codesigned signature to guix.sigs

```sh
pushd ./guix.sigs
git add "${VERSION}/${SIGNER}"/noncodesigned.SHA256SUMS{,.asc}
git commit -m "Add attestations by ${SIGNER} for ${VERSION} non-codesigned"
popd
```

Then open a Pull Request to the [guix.sigs repository](https://github.com/stratum-mining/sv2-tp-guix.sigs).

## Codesigning

### macOS codesigner only: Create detached macOS signatures (assuming [signapple](https://github.com/achow101/signapple/) is installed and up to date with master branch)

In the `guix-build-${VERSION}/output/x86_64-apple-darwin` and `guix-build-${VERSION}/output/arm64-apple-darwin` directories:

    tar xf bitcoin-${VERSION}-${ARCH}-apple-darwin-codesigning.tar.gz
    ./detached-sig-create.sh /path/to/codesign.p12 /path/to/AuthKey_foo.p8 uuid
    Enter the keychain password and authorize the signature
    signature-osx.tar.gz will be created

### Windows codesigner only: Create detached Windows signatures

In the `guix-build-${VERSION}/output/x86_64-w64-mingw32` directory:

    tar xf bitcoin-${VERSION}-win64-codesigning.tar.gz
    ./detached-sig-create.sh /path/to/codesign.key
    Enter the passphrase for the key when prompted
    signature-win.tar.gz will be created

### Windows and macOS codesigners only: test code signatures
It is advised to test that the code signature attaches properly prior to tagging by performing the `guix-codesign` step.
However if this is done, once the release has been tagged in the bitcoin-detached-sigs repo, the `guix-codesign` step must be performed again in order for the guix attestation to be valid when compared against the attestations of non-codesigner builds. The directories created by `guix-codesign` will need to be cleared prior to running `guix-codesign` again.

### Windows and macOS codesigners only: Commit the detached codesign payloads

```sh
pushd ./bitcoin-detached-sigs
# checkout or create the appropriate branch for this release series
git checkout --orphan <branch>
# if you are the macOS codesigner
rm -rf osx
tar xf signature-osx.tar.gz
# if you are the windows codesigner
rm -rf win
tar xf signature-win.tar.gz
git add -A
git commit -m "<version>: {osx,win} signature for {rc,final}"
git tag -s "v${VERSION}" HEAD
git push the current branch and new tag
popd
```

### Non-codesigners: wait for Windows and macOS detached signatures

- Once the Windows and macOS builds each have 3 matching signatures, they will be signed with their respective release keys.
- Detached signatures will then be committed to the [sv2-tp-detached-sigs](https://github.com/stratum-mining/sv2-tp-detached-sigs) repository, which can be combined with the unsigned apps to create signed binaries.

### Create the codesigned build outputs

- [Codesigning build outputs](/contrib/guix/README.md#codesigning-build-outputs)

### Verify other builders' signatures to your own (optional)

- [Verifying build output attestations](/contrib/guix/README.md#verifying-build-output-attestations)

### Commit your codesigned signature to guix.sigs (for the signed macOS/Windows binaries)

```sh
pushd ./guix.sigs
git add "${VERSION}/${SIGNER}"/all.SHA256SUMS{,.asc}
git commit -m "Add attestations by ${SIGNER} for ${VERSION} codesigned"
popd
```

Then open a Pull Request to the [guix.sigs repository](https://github.com/stratum-mining/sv2-tp-guix.sigs).

## After 1 or more people have guix-built and their results match

After verifying signatures, combine the `all.SHA256SUMS.asc` file from all signers into `SHA256SUMS.asc`:

```bash
cat "$VERSION"/*/all.SHA256SUMS.asc > SHA256SUMS.asc
```


- Upload to Github release:
    1. The contents of each `./sv2-tp/guix-build-${VERSION}/output/${HOST}/` directory.

       Guix will output all of the results into host subdirectories, but the SHA256SUMS
       file does not include these subdirectories. In order for downloads via torrent
       to verify without directory structure modification, all of the uploaded files
       need to be in the same directory as the SHA256SUMS file.

       Wait until all of these files have finished uploading before uploading the SHA256SUMS(.asc) files.

    2. The `SHA256SUMS` file

    3. The `SHA256SUMS.asc` combined signature file you just created.

- Update repositories

  - Delete post-EOL [release branches](https://github.com/stratum-mining/sv2-tp/branches/all) and create a tag `v${branch_name}-final`.

  - Create a [new GitHub release](https://github.com/stratum-mining/sv2-tp/releases/new) using the release notes from the tag

- Announce the release:

  - SRI Discord

  - bitcoin-dev mailing list

  - Celebrate

