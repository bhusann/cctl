# Git Instructions — testernice

## Setup (already done)
```bash
git init
git add .
git commit -m "Initial commit"
```

## Daily Workflow

### Check status
```bash
git status                  # what's changed
git diff                    # see unstaged changes
git diff --cached           # see staged changes
git log --oneline -10       # recent commits
```

### Stage and commit
```bash
git add cctl.c                # stage one file
git add -p                  # stage interactively (pick chunks)
git commit -m "fix: description"
```

### Push (when remote is set up)
```bash
git remote add origin git@github.com:user/testernice.git
git push -u origin master
```

## Common Commands

### Undo changes
```bash
git checkout -- cctl.c        # discard unstaged changes to file
git reset HEAD cctl.c         # unstage a file
git reset --soft HEAD~1     # undo last commit, keep changes staged
git reset --hard HEAD~1     # undo last commit AND discard changes (DANGER)
```

### Branches
```bash
git branch                  # list branches
git branch dev              # create branch
git checkout dev            # switch to branch
git checkout -b dev         # create + switch
git merge dev               # merge dev into current branch
git branch -d dev           # delete branch
```

### Stash (save changes temporarily)
```bash
git stash                   # save uncommitted changes
git stash list              # list stashes
git stash pop               # apply + remove latest stash
git stash drop              # delete latest stash
```

### Tags
```bash
git tag v1.0                # tag current commit
git tag v1.0 <commit-hash>  # tag a specific commit
git push --tags             # push tags to remote
```

## .gitignore
Already set up to ignore:
- `cctl` — compiled binary
- `legacygpu.ko` — compiled kernel module
- `*.o` — object files
- `*.bak` — backup files
- `.kernel_version` — build marker

## Tips
- Commit often, commit small
- Write clear commit messages: `fix:`, `feat:`, `refactor:`, `docs:`
- Run `git diff` before committing to review what you're about to save
- Never commit secrets, API keys, or passwords
