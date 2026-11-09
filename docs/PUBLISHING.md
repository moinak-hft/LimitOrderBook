# Create a public GitHub repository

These commands publish the current local project. The README, examples, and build instructions are intended for a public audience. Build directories, generated reports, local hardware inventory, and scratch files are excluded by `.gitignore`. Curated synthetic examples and numeric benchmark evidence are in `examples/`.

## Install GitHub CLI on Windows

If `gh --version` is unavailable, install the official CLI:

```powershell
winget install --id GitHub.cli --source winget
```

Close and reopen PowerShell after installation so the updated PATH is available. See the [official Windows installation instructions](https://github.com/cli/cli/blob/trunk/docs/install_windows.md).

## Sign in

Sign in using the account that should own the repository. The workflow scope allows the included GitHub Actions workflow to be uploaded.

```powershell
gh auth login --hostname github.com --git-protocol https --web --scopes workflow
gh auth setup-git
gh auth status
```

## Commit the public version

Open PowerShell in the project directory. For a fresh downloaded ZIP, initialize Git first with `git init`. An existing local repository can keep its history.

```powershell
git branch -M main
git add .
git diff --cached --stat
git commit -m "Prepare Replay Lab for public release"
```

If Git requests an identity, set `git config user.name "Your Name"` and `git config user.email "YOUR_VERIFIED_OR_GITHUB_NOREPLY_EMAIL"`, then retry the commit. If Git reports nothing to commit, continue using the existing commit.

## Create, connect, and upload

Replace `YOUR_GITHUB_USERNAME` with the account shown by `gh auth status`. The repository name below is `LimitOrderBook`.

Run one command at a time and stop if a command fails:

```powershell
$repository = "YOUR_GITHUB_USERNAME/LimitOrderBook"
gh repo create $repository --public --description "Deterministic C++20 matching engine with execution replay, strategy experiments, and multicore benchmarks"
```

After repository creation succeeds, connect the local Git remote. This works whether an `origin` remote was configured earlier or not:

```powershell
$repositoryUrl = "https://github.com/$repository.git"
if ((git remote) -contains 'origin') {
    git remote set-url origin $repositoryUrl
} else {
    git remote add origin $repositoryUrl
}
git push --set-upstream origin main
gh repo view $repository --json nameWithOwner,visibility,url
```

The last command should show `"visibility": "PUBLIC"`. The first push triggers the build-and-test workflow for Linux, Windows, and macOS. Check the Actions tab for its actual results; local Windows checks do not establish that hosted jobs passed.

Creating an empty remote first and then pushing reuses the existing local README and commit history. Reference: [GitHub CLI repository creation](https://cli.github.com/manual/gh_repo_create) and [authentication](https://cli.github.com/manual/gh_auth_login).

## Later updates

```powershell
git add .
git commit -m "Describe the change"
git push
```

See [attribution](ATTRIBUTION.md) for the preserved original source and supplied licensing information. No project-wide license has been invented or attached to the inherited code.
