# Hosting the Smart Timer app

The repository can host the **Smart Timer** Bluetooth PWA on GitHub Pages.
This is the V2 browser controller, not the firmware's HTTP server. A hosted
page talks directly to a nearby V2 timer through Web Bluetooth; it does not
provide Internet remote control, relay service, or Bluetooth support for V1.

## Enable deployment

1. Use a public source repository with the Pages workflow enabled. Keep
   credentials, private captures and generated build inputs out of Git.
2. In **Settings → Pages → Build and deployment → Source**, choose
   **GitHub Actions**.
3. The `published` event for a version-tagged GitHub release triggers
   **Smart Timer web app**. Its attached artifacts must satisfy the
   [release artifact contract](BUILDING.md#release-artifacts).
   Branch pushes and drafts do not deploy. Every published version tag uses
   the same feed; there are no separate RC/beta channels.
4. Use the deployment URL shown by the workflow. The app lives at
   `https://<account>.github.io/<repository>/bluetooth-app/`.

The site root is a short home page from `pages/`: what the app is, a button to
open it, the V1 note, the safety and battery notice with a link to `SAFETY.md`,
and links to the source and releases. It has its own Chinese text in
`pages/home.js`, so none of it is built into timer firmware. The app's offline
service worker is scoped to `/bluetooth-app/` and never controls the home page.
The same site serves the signed update feed under `updates/` for every timer,
V1 included; see [GitHub updates](GITHUB_UPDATES.md).

The workflow checks out the published release's tag—not the latest default
branch—and verifies its attached images, checksums, version and source commit
against `BUILD.json` and `release.json`, and verifies `updates.json` against the
configured public release key. Feed selection includes every published valid
version tag, ordered by SemVer. Companion source follows that same newest
version, including a tagged prerelease.
It then runs `scripts/check.sh all` and stages an explicit allowlist with
`scripts/build/build_pages.py`. Verified signed firmware feeds and application
images are staged separately under `updates/`; see [GitHub updates](GITHUB_UPDATES.md).
Only `dist/pages/` is uploaded. Relay code, private inputs, signing keys
and firmware symbols are excluded.
The app's Source code link points at the
exact public Git commit used for the deployment. The footer also shows **Web
app build**, its version and a clickable 12-character commit hash; the full
SHA is in the link. This identifies the browser app, not the connected timer's
firmware. A raw development checkout says **Development checkout** instead of
inventing a commit. The label is cached with its matching offline app shell.
The license and licensing
notices remain in the repository, not in the app interface or Pages artifact.
See [source and relinking materials](BUILDING.md#source-and-relinking-materials)
for the separate firmware source distribution.

Do not select “deploy from branch” with the whole repository as the site root.
Artifact allowlisting protects Pages output, not files you commit to GitHub:
review the repository itself for private material before pushing it.

Enable the workflow on the default branch before publishing the first release.
The deployment runs from the release **tag**, and GitHub creates the
`github-pages` environment allowing only the default branch. In **Settings →
Environments → github-pages → Deployment branches and tags**, add a tag rule
matching `v*` (keep the default-branch rule). Without it the deploy job is
rejected by environment protection and no update feed is published.
Publish through the GitHub UI (or a separately authorized human token), not the
workflow's `GITHUB_TOKEN`: events created by that token generally do not start
another workflow. A failed Pages deployment can be retried from its Actions
run; it does not require rebuilding/replacing firmware assets. If deploying a
different release intentionally, qualify that version of the app too.
See GitHub's [workflow-trigger rules](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/trigger-a-workflow)
for the token restriction.

## Local packaging check

From a Git checkout, use an empty destination:

```sh
python3 scripts/build/build_pages.py \
  --repository YOUR_ACCOUNT/YOUR_REPOSITORY \
  --revision "$(git rev-parse HEAD)" \
  --output dist/pages-preview
python3 -m http.server 8000 --bind 127.0.0.1 --directory dist/pages-preview
```

Open `http://127.0.0.1:8000/bluetooth-app/`. The packager deliberately refuses
a nonempty destination rather than deleting files. Use another destination
for a subsequent preview. Packaging and deployment never flash a timer.

Successful CI also retains `smart-timer-web-app-preview` for 14 days. Download
and serve its extracted root on localhost to inspect the release candidate's
stamped app without changing the production site. Use the same repository and
full commit when packaging locally so the app bytes/cache revision match CI.

## Browser identity and offline use

Use a Web Bluetooth-capable browser, such as Chrome on Android. HTTPS is
required except for localhost development. Installing a PWA does not add Web
Bluetooth to browsers that lack it.

Keep the production origin and app path stable. Pairing credentials stay in
browser storage, but changing the origin or clearing site data requires
recovering or re-pairing that browser. The site operator can change the
JavaScript served at that origin: use a trusted deployment and protect its
repository and deployment permissions. Pages hosting is not a security
boundary against the operator of that page.

Load the app online once and wait for its offline-ready message before relying
on offline launch. Each published shell gets a content-derived cache version;
installation caches the complete shell atomically. An update waits until all
old app windows close, rather than replacing files during a BLE session.
Close every app window and reopen to activate an available update. Failed
downloads leave the previous installed shell usable. The linked source
repository is not part of the offline app shell.

## Release boundaries

Publishing the browser app does not establish physical-device behavior.
Deployed app source follows the newest published
version tag, and the Pages job verifies its publication metadata and attached
artifacts. Automated deployment does not observe hardware testing. Original
project code uses
AGPL-3.0-only; third-party licenses remain separate. See
[third-party notices](../THIRD_PARTY_NOTICES.md) and [safety notice](../SAFETY.md).

GitHub's official instructions: [custom Pages workflows](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages).
