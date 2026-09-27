# hyprtail: standing rules

Apply to every session in this repo.

- **Testing happens on the host**, through `hyprpm update` and
  `hyprpm reload -f`. Give host test instructions only.
- **No nested-instance testing** unless a change carries a real stability
  risk (for example new GL resource handling or hooks). When that's the
  case, say so explicitly and explain why.
- **Never try to reproduce or visually verify rendering yourself.** The user
  tests and reports back.
- **Don't load or run anything in the host or nested instances.**
- **Don't commit or push.**

Also in force from earlier sessions:

- Report the plan before writing code for anything non-trivial.
- Cite file and line for any Hyprland or hyprutils behavior relied on, at the
  pinned commit (SPEC §2). If a dependency's source isn't available to read,
  say so instead of describing what it probably does.
- SPEC.md is the design authority; NOTES.md holds reasoning and citations.
