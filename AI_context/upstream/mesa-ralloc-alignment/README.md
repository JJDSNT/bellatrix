# Mesa MR: ralloc alignment

Two commits for Mesa, made against 26.2.5. Both apply cleanly to Mesa `main`
as of 2026-10-26. Background and test results are in
`docs/MesaRallocAlignment.md`. `MR-description.md` is the text for the MR.

Mesa does not take GitHub pull requests. MRs go through
gitlab.freedesktop.org, and these steps need the maintainer's own account:

1. Create an account on gitlab.freedesktop.org. New accounts cannot fork
   until they are verified, which is done by filing the "user verification"
   issue in the `freedesktop/freedesktop` project.
2. Fork `mesa/mesa`, then:

   ```bash
   git clone git@ssh.gitlab.freedesktop.org:<user>/mesa.git && cd mesa
   git switch -c ralloc-alignment origin/main
   git am /path/to/bellatrix/AI_context/upstream/mesa-ralloc-alignment/*.patch
   git push -u origin ralloc-alignment
   ```

3. Open the MR against `mesa/mesa` `main` and paste `MR-description.md`.
   Mesa CI runs on the MR. Merging is done by the Marge bot once a reviewer
   gives `Reviewed-by`.

Before submitting, check Mesa's current policy on AI-assisted contributions
(`docs/submittingpatches.rst`).

Once it is merged, remove the `ralloc.c` hunk from
`aros/workbench/libs/mesa/bellatrix/mesa-<version>-bellatrix.diff` at the next
Mesa bump.
