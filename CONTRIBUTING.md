# Contributing to iDared 32bit

iDared 32bit takes contributions as ordinary GitHub pull requests. There is no separate code review system, mailing list or sign-up: fork the repository, make your changes and open a pull request.

iDared 32bit is independent of touchHLE. Please send issues and pull requests for iDared 32bit here, not to the touchHLE project.

Please also read the [code of conduct](CODE_OF_CONDUCT.md).

## Issues

There are a great many apps that don't work yet, so please only open an issue about a broken app if one of these applies:

- It worked in an earlier version of iDared 32bit, and you can reproduce the difference.
- It partly works (for example, it reaches the menu but the game itself fails). An app's splash screen (`Default.png`) appearing doesn't count as partly working.

Include the iDared 32bit version, your device and iOS version, what you did, what happened, and the log if you have it.

## Pull requests

### Getting the code

```
$ git clone https://github.com/iDared32bit-emu/iDared32bit.git
$ cd iDared32bit
```

To contribute, [fork the repository](https://github.com/iDared32bit-emu/iDared32bit/fork) on GitHub, push a branch to your fork, and open a pull request against the default branch.

### Before you open one

- Read [the building guide](dev-docs/building.md) and [the coding style guide](dev-docs/code-style.md). More developer documentation is in [`dev-docs`](dev-docs/) and throughout the code.
- Run [`dev-scripts/format.sh`](dev-scripts/format.sh) and [`dev-scripts/lint.sh`](dev-scripts/lint.sh).
- Run `cargo test`. [Building the integration tests needs LLVM and a custom toolchain](tests/README.md); if you don't have them, skip those tests with `cargo test -- --skip test_app` and let CI run them.

GitHub Actions runs formatting, lint and the tests on every pull request.

### What makes a pull request easy to merge

- Keep it focused. One pull request per fix or feature, split into commits that each make sense on their own.
- Explain what it changes and why, and how you tested it. For compatibility fixes, name the app and what now works.
- If you're planning something large, open an issue first so we can agree on the approach before you spend the time.

Fixes found in review can go in as extra commits; they're squashed or tidied when the pull request is merged. Contributions are accepted under the [Mozilla Public License, version 2.0](LICENSE), the same license as the rest of the project.

## Copyright and reverse engineering

(Please also read the copyright rules in the code of conduct.)

⚠️ Be **very** careful about copyright. **Don't contribute if you've seen code you shouldn't have seen, don't copy code that isn't yours to copy, and never _secretly_ copy and pretend you didn't.** Infringing Apple's or anyone else's copyright could end the project. **If in doubt, don't.** In particular:

* ⚠️ When implementing an API, rely first and foremost on public documentation.
* ⚠️ Never look at or rely on _leaked_ code, documentation, tools and so on. Being available somewhere doesn't make something open source.
* ⚠️ Don't disassemble or decompile components of iPhone OS or other Apple platforms. If you can't find out how an API behaves any other way, don't implement it.
* ⚠️ Header files may be used only as a source of simple facts, such as a constant's value or what a type alias resolves to. Don't copy their layout or organisation, anything you don't need, or names that aren't part of the ABI or public API.
* ⚠️ Open-source code is still covered by copyright. Avoid reading other implementations unless there's no alternative, and don't copy their algorithms. Code under a compatible license can sometimes be brought in _under that license, as a dependency_ instead.
* ⚠️ If you work or have worked at Apple, NeXT or a similar organisation, and may have seen proprietary iPhone OS source code, please don't contribute.
* ⚠️ If your employment contract or local law means you may not own the copyright in what you write, get your employer's permission before contributing.
